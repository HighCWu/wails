//go:build linux

package application

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"slices"
	"sync"
	"time"
)

// nativeBridgeListener hosts the per-instance Unix socket endpoint that the
// renderer's native transport addon dials (fetch-ipc experiment sibling:
// WAILS_ELECTRON_EXPERIMENT=native-ipc). Frames are JSONL
// {"id":N,"payload":"..."} and are echoed back — this measures the pure
// renderer→Go transport (see examples/webview-compat).
var nativeBridge struct {
	mu    sync.Mutex
	path  string
	token string
	ln    net.Listener
}

func startNativeBridgeListener() (string, string, error) {
	dir, err := dirForBridgeSocket()
	if err != nil {
		return "", "", err
	}
	token := make([]byte, 16)
	if _, err := rand.Read(token); err != nil {
		return "", "", err
	}
	tokenHex := hex.EncodeToString(token)
	path := filepath.Join(dir, fmt.Sprintf("wails-compat-%d-%d.sock", os.Getpid(), time.Now().UnixNano()))
	_ = os.Remove(path) // stale file from a dead process is safe to remove
	ln, err := net.Listen("unix", path)
	if err != nil {
		return "", "", err
	}
	_ = os.Chmod(path, 0o600)
	nativeBridge.mu.Lock()
	nativeBridge.path = path
	nativeBridge.ln = ln
	nativeBridge.mu.Unlock()

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go serveNativeBridgeConn(conn, tokenHex)
		}
	}()
	return path, tokenHex, nil
}

func nativeBridgePath() string {
	nativeBridge.mu.Lock()
	defer nativeBridge.mu.Unlock()
	return nativeBridge.path
}

func serveNativeBridgeConn(conn net.Conn, expectedToken string) {
	defer conn.Close()
	// HELLO handshake: the first frame must carry the per-instance token;
	// anything else is a foreign local process and gets dropped silently.
	line, err := readFrame(conn, nil)
	if err != nil {
		return
	}
	var hello struct {
		Hello int    `json:"hello"`
		Token string `json:"token"`
	}
	if err := json.Unmarshal(line, &hello); err != nil ||
		hello.Hello != 1 || hello.Token != expectedToken {
		return
	}
	conn.Write([]byte(`{"ready":true}` + "\n"))
	// Echo benchmark frames verbatim: the sweep measures the pure
	// transport, so the host does not re-parse megabyte payloads here.
	// (A production data plane plugs its own frame handler in here.)
	// A fixed-slice reader would turn a 1MB frame into dozens of read
	// syscalls; a self-managed buffer read straight from the conn keeps
	// the host cost at a handful of reads + one writev per frame.
	buf := make([]byte, 0, 256*1024)
	for {
		line, err := readFrame(conn, &buf)
		if err != nil {
			return
		}
		if len(line) == 0 {
			continue
		}
		if err := writeFrame(conn, line); err != nil {
			return
		}
	}
}

// readFrame reads one '\n'-terminated frame into *buf (grown as needed,
// reused across frames; nil allocates locally). The strict
// request/response link expects one frame at a time — residual bytes
// after a frame are a protocol violation.
func readFrame(conn net.Conn, buf *[]byte) ([]byte, error) {
	var local []byte
	if buf == nil {
		buf = &local
	}
	*buf = (*buf)[:0]
	start := 0
	for {
		if i := bytes.IndexByte((*buf)[start:], '\n'); i >= 0 {
			line := (*buf)[start : start+i]
			rest := start + i + 1
			if rest != len(*buf) {
				return nil, fmt.Errorf("bridge frame residue: %d bytes", len(*buf)-rest)
			}
			*buf = (*buf)[:0]
			return line, nil
		}
		if len(*buf) == cap(*buf) {
			*buf = slices.Grow(*buf, cap(*buf)+1)[:len(*buf)]
		}
		n, err := conn.Read((*buf)[len(*buf):cap(*buf)])
		*buf = (*buf)[:len(*buf)+n]
		if err != nil {
			return nil, err
		}
	}
}

// writeFrame emits the frame plus newline as one aggregated writev.
func writeFrame(conn net.Conn, line []byte) error {
	buffers := net.Buffers{line, []byte{'\n'}}
	_, err := buffers.WriteTo(conn)
	return err
}

func dirForBridgeSocket() (string, error) {
	if runtime := os.Getenv("XDG_RUNTIME_DIR"); runtime != "" {
		return runtime, nil
	}
	return os.TempDir(), nil
}
