//go:build linux

package application

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
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

	"github.com/wailsapp/wails/v3/internal/v8serde"
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

func startNativeBridgeListener(serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) (string, string, error) {
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

func serveNativeBridgeConn(conn net.Conn, expectedToken string, serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) {
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
	// After the (line-framed) HELLO, all frames are protocol v3:
	// 4-byte LE length + raw bytes of a v8-serialized invoke object
	// {id, channel, method, url, bodyLen, body, headers}. Channels:
	// "http" rides the bindings data plane through the asset server;
	// "echo" is answered verbatim (benchmark diagnostics).
	buf := make([]byte, 0, 256*1024)
	for {
		msg, err := readMessage(conn, &buf)
		if err != nil {
			return
		}
		if len(msg) == 0 {
			continue
		}
		decoded, derr := v8serde.Deserialize(msg)
		if derr != nil {
			fmt.Println("[bridge] decode err:", derr)
			return
		}
		obj, ok := decoded.(map[string]any)
		if !ok {
			continue
		}
		channel, _ := obj["channel"].(string)
		if channel == "echo" {
			if err := writeMessage(conn, msg); err != nil {
				return
			}
			continue
		}
		if channel == "http" {
			if err := serveInvokeMessage(conn, obj, serveHTTP); err != nil {
				fmt.Println("[bridge] invoke err:", err)
				return
			}
			continue
		}
	}
}

// serveHTTPFrame handles one {"id":N,"type":"http",...} frame: run the
// request through the asset server and answer with
// {"id":N,"status":..,"contentType":..,"payload":"<base64>"}.
func serveHTTPFrame(conn net.Conn, line []byte, buf *[]byte, serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) {
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
	fmt.Println("[httpframe] recv id=", req.ID, "url=", req.URL, "bodyLen=", req.BodyLen)
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
	var local []byte
	if buf == nil {
		buf = &local // HELLO handshake path passes nil
	}
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

// serveInvokeMessage runs one http-channel invoke through the asset
// server and answers with a v8-serialized response message.
func serveInvokeMessage(conn net.Conn, obj map[string]any, serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) error {
	id, _ := obj["id"].(float64)
	method, _ := obj["method"].(string)
	rawURL, _ := obj["url"].(string)
	body, _ := obj["body"].(string)
	hdr := http.Header{}
	if hm, ok := obj["headers"].(map[string]any); ok {
		for k, v := range hm {
			if sv, ok := v.(string); ok {
				hdr.Set(k, sv)
			}
		}
	}
	if method == "" {
		method = http.MethodGet
	}
	status, contentType, respBody, err := serveHTTP(method, rawURL, body, hdr)
	if err != nil {
		respBody = []byte(err.Error())
		status = 500
		contentType = "text/plain"
	}
	resp, merr := v8serde.Serialize(map[string]any{
		"id":          id,
		"status":      float64(status),
		"contentType": contentType,
		"body":        string(respBody),
	})
	if merr != nil {
		return merr
	}
	return writeMessage(conn, resp)
}

// readMessage reads one length-prefixed message (4-byte LE length +
// bytes). Bytes already buffered in *buf from a previous read's
// look-ahead are consumed first.
func readMessage(conn net.Conn, buf *[]byte) ([]byte, error) {
	var lenBuf [4]byte
	taken := copy(lenBuf[:], *buf)
	copy(*buf, (*buf)[taken:])
	*buf = (*buf)[:len(*buf)-taken]
	if taken < 4 {
		if _, err := io.ReadFull(conn, lenBuf[taken:]); err != nil {
			return nil, err
		}
	}
	n := binary.LittleEndian.Uint32(lenBuf[:])
	msg := make([]byte, n)
	taken = copy(msg, *buf)
	copy(*buf, (*buf)[taken:])
	*buf = (*buf)[:len(*buf)-taken]
	if uint32(taken) < n {
		if _, err := io.ReadFull(conn, msg[taken:]); err != nil {
			return nil, err
		}
	}
	return msg, nil
}

// writeMessage emits one length-prefixed message.
func writeMessage(conn net.Conn, msg []byte) error {
	var lenBuf [4]byte
	binary.LittleEndian.PutUint32(lenBuf[:], uint32(len(msg)))
	if _, err := conn.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err := conn.Write(msg)
	return err
}

func dirForBridgeSocket() (string, error) {
	if runtime := os.Getenv("XDG_RUNTIME_DIR"); runtime != "" {
		return runtime, nil
	}
	return os.TempDir(), nil
}
