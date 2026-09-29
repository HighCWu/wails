// Package electron drives an official prebuilt Electron runtime as a Wails
// webview backend. Electron is an out-of-process runtime: this package
// spawns the Electron main process, exchanges a JSON-lines control protocol
// over stdin/stdout, and exposes window operations as plain method calls.
//
// The bootstrap application (assets/main.js) owns every BrowserWindow; the
// renderer bridge (assets/preload.js) shims window.chrome.webview.postMessage
// so the Wails JS runtime keeps working unmodified.
package electron

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

// FindRuntime resolves the Electron executable: WAILS_ELECTRON_DIR must
// point at an Electron distribution directory (containing the electron
// binary). There is no silent fallback — an unusable runtime is an error.
func FindRuntime() (string, error) {
	dir := strings.TrimSpace(os.Getenv("WAILS_ELECTRON_DIR"))
	if dir == "" {
		return "", errors.New(
			"electron backend: WAILS_ELECTRON_DIR is not set; point it at an Electron distribution directory")
	}
	exe := "electron"
	if runtime.GOOS == "windows" {
		exe = "electron.exe"
	}
	full := filepath.Join(dir, exe)
	if st, err := os.Stat(full); err != nil || st.IsDir() {
		return "", fmt.Errorf("electron backend: %s not found (WAILS_ELECTRON_DIR=%s)", full, dir)
	}
	return full, nil
}

// Event is an asynchronous notification from the Electron main process:
// window lifecycle events and renderer postMessage payloads.
type Event struct {
	Name     string
	WindowID uint
	Params   json.RawMessage
}

// RequestHandler processes a request originating from the Electron main
// process (e.g. a /wails/runtime call forwarded by the preload shim). The
// returned value is marshalled back as the response result.
type RequestHandler func(method string, params json.RawMessage) (any, error)

// Process is a running Electron main process speaking the JSON-lines
// control protocol.
type Process struct {
	cmd *exec.Cmd

	writeMu sync.Mutex
	stdin   *bufio.Writer
	stdinP  io.WriteCloser

	mu      sync.Mutex
	nextID  uint64
	pending map[uint64]chan callResult

	handlerMu sync.Mutex
	handler   RequestHandler

	eventsMu sync.Mutex
	events   []chan Event

	dead bool
	done chan struct{}
}

type callResult struct {
	result json.RawMessage
	err    error
}

type wireMessage struct {
	T   string          `json:"t"`
	ID  uint64          `json:"id,omitempty"`
	M   string          `json:"m,omitempty"`
	Ok  bool            `json:"ok,omitempty"`
	R   json.RawMessage `json:"r,omitempty"`
	E   string          `json:"e,omitempty"`
	Err string          `json:"err,omitempty"`
	P   json.RawMessage `json:"p,omitempty"`
}

type jsonEncoder interface {
	Encode(any) error
}

// Start launches the Electron executable with the extracted bootstrap and
// wires up the control protocol. cfg is delivered to the bootstrap via the
// WAILS_ELECTRON_CONFIG environment variable.
func Start(exe, bootstrap, preload string, extraSwitches []string, cfg map[string]any) (*Process, error) {
	configJSON, err := json.Marshal(map[string]any{
		"assetsURL": cfg["assetsURL"],
		"preload":   preload,
	})
	if err != nil {
		return nil, err
	}

	args := append(append([]string{}, extraSwitches...), bootstrap)
	cmd := exec.Command(exe, args...)
	cmd.Env = append(os.Environ(),
		"WAILS_ELECTRON_CONFIG="+string(configJSON),
	)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = os.Stderr

	p := &Process{
		cmd:     cmd,
		stdin:   bufio.NewWriter(stdin),
		stdinP:  stdin,
		pending: make(map[uint64]chan callResult),
		done:    make(chan struct{}),
	}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("electron backend: starting %s: %w", exe, err)
	}

	// Orphan protection, layer 1: a standalone shell watchdog. When the host
	// dies abruptly the Electron main process's own event loop can wedge
	// (timers stop, SIGTERM handler can't complete), so an in-process PPID
	// guard is not enough — an orphaned shell keeps running and kills the
	// whole Electron tree once the host PID is gone. Non-Windows only; the
	// Windows path is handled at job teardown.
	if runtime.GOOS != "windows" {
		hostPid := os.Getpid()
		epid := cmd.Process.Pid
		watchdog := exec.Command("/bin/sh", "-c", fmt.Sprintf(
			"while kill -0 %d 2>/dev/null; do sleep 2; done; kill -TERM %d 2>/dev/null; sleep 3; kill -9 %d 2>/dev/null",
			hostPid, epid, epid))
		if err := watchdog.Start(); err != nil {
			cmd.Process.Kill()
			return nil, fmt.Errorf("electron backend: watchdog: %w", err)
		}
		// The watchdog lives exactly as long as needed; when the host exits
		// normally, Shutdown kills electron and the watchdog loop terminates.
		go func() {
			<-p.done
			watchdog.Process.Kill()
		}()
	}

	go p.readLoop(stdout)
	go func() {
		// stdin closing (our Shutdown or our death) makes the bootstrap quit —
		// the orphan-process protection.
		_ = cmd.Wait()
		p.mu.Lock()
		p.dead = true
		for id, ch := range p.pending {
			ch <- callResult{err: errors.New("electron process exited")}
			delete(p.pending, id)
		}
		p.mu.Unlock()
		close(p.done)
	}()

	return p, nil
}

// SetRequestHandler installs the handler for requests sent by the Electron
// main process. Must be called before Start returns processing begins.
func (p *Process) SetRequestHandler(h RequestHandler) {
	if debugEnabled() {
		fmt.Fprintf(os.Stderr, "[wails-electron pid=%d] request handler installed\n", os.Getpid())
	}
	p.handlerMu.Lock()
	p.handler = h
	p.handlerMu.Unlock()
}

func (p *Process) getRequestHandler() RequestHandler {
	p.handlerMu.Lock()
	defer p.handlerMu.Unlock()
	return p.handler
}

func debugEnabled() bool {
	return strings.TrimSpace(os.Getenv("WAILS_ELECTRON_DEBUG")) == "1"
}

// respond sends a response for a host-handled request back to Electron.
func (p *Process) respond(id uint64, result any, err error) {
	if err != nil {
		p.send(wireMessage{T: "resp", ID: id, Ok: false, Err: err.Error()})
		return
	}
	raw, mErr := json.Marshal(result)
	if mErr != nil {
		p.send(wireMessage{T: "resp", ID: id, Ok: false, Err: mErr.Error()})
		return
	}
	p.send(wireMessage{T: "resp", ID: id, Ok: true, R: raw})
}

func (p *Process) send(m wireMessage) {
	raw, err := json.Marshal(m)
	if err != nil {
		return
	}
	p.writeMu.Lock()
	_, _ = fmt.Fprintln(p.stdin, string(raw))
	_ = p.stdin.Flush()
	p.writeMu.Unlock()
}

func (p *Process) readLoop(stdout interface{ Read([]byte) (int, error) }) {
	scanner := bufio.NewScanner(stdout)
	scanner.Buffer(make([]byte, 0, 1024*1024), 16*1024*1024)
	for scanner.Scan() {
		line := scanner.Bytes()
		if len(strings.TrimSpace(string(line))) == 0 {
			continue
		}
		var m wireMessage
		if err := json.Unmarshal(line, &m); err != nil {
			continue
		}
		switch m.T {
		case "resp":
			p.mu.Lock()
			ch, ok := p.pending[m.ID]
			if ok {
				delete(p.pending, m.ID)
			}
			p.mu.Unlock()
			if ok {
				if m.Ok {
					ch <- callResult{result: m.R}
				} else {
					ch <- callResult{err: errors.New(m.Err)}
				}
			}
		case "req":
			handler := p.getRequestHandler()
			if handler == nil {
				if debugEnabled() {
					fmt.Fprintf(os.Stderr, "[wails-electron pid=%d] NO HANDLER for %s\n", os.Getpid(), m.M)
				}
				p.respond(m.ID, nil, errors.New("no request handler installed"))
				continue
			}
			id := m.ID
			go func() {
				result, err := handler(m.M, m.P)
				p.respond(id, result, err)
			}()
		case "ev":
			p.emitEvent(Event{Name: m.E, Params: m.P, WindowID: windowIDFromParams(m.P)})
		}
	}
}

func windowIDFromParams(params json.RawMessage) uint {
	if len(params) == 0 {
		return 0
	}
	var probe struct {
		ID      uint            `json:"id"`
		Payload json.RawMessage `json:"payload"`
	}
	_ = json.Unmarshal(params, &probe)
	return probe.ID
}

func (p *Process) emitEvent(ev Event) {
	p.eventsMu.Lock()
	for _, ch := range p.events {
		select {
		case ch <- ev:
		default: // never block the reader on a slow consumer
		}
	}
	p.eventsMu.Unlock()
}

// Subscribe registers another event receiver; Shutdown closes the channels.
func (p *Process) Subscribe() <-chan Event {
	ch := make(chan Event, 256)
	p.eventsMu.Lock()
	p.events = append(p.events, ch)
	p.eventsMu.Unlock()
	return ch
}

// Call performs a control round trip. OK results decode into out (when
// non-nil); errors include the bootstrap-side message.
func (p *Process) Call(method string, windowID uint, params map[string]any, out any) error {
	if params == nil {
		params = map[string]any{}
	}
	if windowID != 0 {
		params["id"] = windowID
	}
	p.mu.Lock()
	if p.dead {
		p.mu.Unlock()
		return errors.New("electron process has exited")
	}
	p.nextID++
	id := p.nextID * 2 // even ids: Go-initiated (odd ids are Electron-initiated)
	ch := make(chan callResult, 1)
	p.pending[id] = ch
	p.mu.Unlock()

	paramsJSON, err := json.Marshal(params)
	if err != nil {
		return err
	}
	raw, err := json.Marshal(wireMessage{T: "req", ID: id, M: method, P: paramsJSON})
	if err != nil {
		return err
	}
	p.writeMu.Lock()
	_, err = fmt.Fprintln(p.stdin, string(raw))
	err2 := p.stdin.Flush()
	p.writeMu.Unlock()
	if err != nil || err2 != nil {
		p.mu.Lock()
		delete(p.pending, id)
		p.mu.Unlock()
		return fmt.Errorf("electron ipc write: %w", errors.Join(err, err2))
	}

	select {
	case res := <-ch:
		if res.err != nil {
			return res.err
		}
		if out != nil && len(res.result) > 0 {
			return json.Unmarshal(res.result, out)
		}
		return nil
	case <-time.After(15 * time.Second):
		p.mu.Lock()
		delete(p.pending, id)
		p.mu.Unlock()
		return fmt.Errorf("electron ipc timeout: %s", method)
	case <-p.done:
		return errors.New("electron process has exited")
	}
}

// Shutdown closes stdin (the bootstrap quits on EOF), waits briefly, then
// kills the process if it is still alive.
func (p *Process) Shutdown() {
	_ = p.stdinP.Close()
	select {
	case <-p.done:
	case <-time.After(5 * time.Second):
		_ = p.cmd.Process.Kill()
	}
}
