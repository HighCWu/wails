//go:build linux

package application

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"sync"
	"time"
)

// nativeBridgeListener hosts the per-instance Unix socket endpoint that
// the renderer's native transport addon dials. Frames are JSONL:
//
//	{"id":N,"payload":"..."}                                 -> echo (benchmark)
//	{"id":N,"type":"http","method":..,"url":..,"body":"..."}  -> /wails/runtime
//
// The http form is the promoted bindings data plane: renderer fetch ->
// UDS -> asset server in-process — no network stack, no Electron IPC.
var nativeBridge struct {
	mu    sync.Mutex
	path  string
	token string
	ln    net.Listener
}

func startNativeBridgeListener(serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, string, error)) (string, string, error) {
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
			go serveNativeBridgeConn(conn, tokenHex, serveHTTP)
		}
	}()
	return path, tokenHex, nil
}

func nativeBridgePath() string {
	nativeBridge.mu.Lock()
	defer nativeBridge.mu.Unlock()
	return nativeBridge.path
}

func serveNativeBridgeConn(conn net.Conn, expectedToken string, serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, string, error)) {
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
	// http frames are the promoted bindings data plane — they carry the
	// /wails/runtime request and get a structured response frame.
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
		// the http marker travels inside the JSON-escaped payload string
		if bytes.Contains(line, []byte("\\\"type\\\":\\\"http\\\"")) {
			serveHTTPFrame(conn, line, &buf, serveHTTP)
			continue
		}
		if err := writeFrame(conn, line); err != nil {
			return
		}
	}
}

// serveHTTPFrame handles one {"id":N,"type":"http",...} frame: run the
// request through the asset server and answer with
// {"id":N,"status":..,"contentType":..,"payload":"<base64>"}.
func serveHTTPFrame(conn net.Conn, line []byte, buf *[]byte, serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, string, error)) {
	var req struct {
		ID      int               `json:"id"`
		Method  string            `json:"method"`
		URL     string            `json:"url"`
		BodyLen int               `json:"bodyLen"`
		Headers map[string]string `json:"headers"`
	}
	if err := json.Unmarshal(line, &req); err != nil {
		fmt.Println("[httpframe] unmarshal err:", err)
		return
	}
	body, err := readExactBody(conn, buf, req.BodyLen)
	if err != nil {
		fmt.Println("[httpframe] body read err:", err)
		return
	}
	hdr := http.Header{}
	for k, v := range req.Headers {
		hdr.Set(k, v)
	}
	if req.Method == "" {
		req.Method = http.MethodGet
	}
	status, contentType, bodyB64, err := serveHTTP(req.Method, req.URL, string(body), hdr)
	respBody := []byte(bodyB64)
	if err != nil {
		respBody = []byte(err.Error())
		status = 500
		contentType = "text/plain"
	}
	respHdr, _ := json.Marshal(map[string]any{
		"id":          req.ID,
		"status":      status,
		"contentType": contentType,
		"bodyLen":     len(respBody),
	})
	if err := writeFrame(conn, respHdr); err != nil {
		return
	}
	if _, err := conn.Write(respBody); err != nil {
		fmt.Println("[httpframe] body write err:", err)
	}
}

// readFrame reads one '\n'-terminated frame into *buf (grown as needed,
// reused across frames; nil allocates locally). The strict
// request/response link expects one frame at a time — residual bytes
// after a frame are a protocol violation.
// readFrame reads one '\n'-terminated header line into *buf (reused
// across calls). Bytes after the newline are the frame's raw body
// (protocol v2) — they stay at the front of *buf for readExactBody to
// consume; callers of plain echo frames see an empty residue as before.
func readFrame(conn net.Conn, buf *[]byte) ([]byte, error) {
	start := 0
	for {
		if i := bytes.IndexByte((*buf)[start:], '\n'); i >= 0 {
			line := (*buf)[start : start+i]
			rest := start + i + 1
			consumed := rest
			// move the residue (start of the body) to the front
			copy(*buf, (*buf)[rest:])
			*buf = (*buf)[:len(*buf)-rest]
			_ = consumed
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

// readExactBody consumes n bytes: whatever the reader already pulled
// into buf (the body's first bytes may precede the header's newline),
// then straight from the connection for the remainder. Returns a copy.
func readExactBody(conn net.Conn, buf *[]byte, n int) ([]byte, error) {
	out := make([]byte, n)
	taken := copy(out, (*buf)[:min(len(*buf), n)])
	copy(*buf, (*buf)[taken:])
	*buf = (*buf)[:len(*buf)-taken]
	for taken < n {
		m, err := conn.Read(out[taken:n])
		if m > 0 {
			taken += m
		}
		if err != nil {
			return nil, err
		}
		if m == 0 {
			return nil, io.EOF
		}
	}
	return out, nil
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
