//go:build linux

package application

import (
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
	// Every frame on this connection is protocol v3: 4-byte LE length +
	// payload. The HELLO handshake is the first such frame and must carry
	// the per-instance token; anything else is a foreign local process
	// and gets dropped silently.
	buf := make([]byte, 0, 256*1024)
	line, err := readMessage(conn, &buf)
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
	writeMessage(conn, []byte(`{"ready":true}`))
	// All frames after HELLO are v8-serialized invoke objects
	// {id, channel, method, url, bodyLen, body, headers}. Channels:
	// "http" rides the bindings data plane through the asset server;
	// "echo" is answered verbatim (benchmark diagnostics).
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

// isV8SerializedBody reports whether the call body carries the V8
// ValueSerializer version header (0xFF + small varint version — 0x0F on
// current Node, 0x10 on Electron's Node 24). JSON bodies never start
// with 0xFF, so the check is unambiguous.
func isV8SerializedBody(body string) bool {
	if len(body) < 2 || body[0] != 0xFF {
		return false
	}
	v := body[1]
	return v >= 0x01 && v <= 0x1F
}

// serveInvokeMessage runs one http-channel invoke through the asset
// server and answers with a v8-serialized response message.
func serveInvokeMessage(conn net.Conn, obj map[string]any, serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) error {
	id, _ := obj["id"].(float64)
	method, _ := obj["method"].(string)
	rawURL, _ := obj["url"].(string)
	var body string
	switch b := obj["body"].(type) {
	case string:
		body = b
	case []byte: // v8-body mode: the call body arrives as raw bytes
		body = string(b)
	}
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
	// The runtime (with __wailsV8Body gating) sends the call object
	// v8-serialized: decode it back to the JSON shape the bindings
	// pipeline consumes. Plain JSON bodies pass through untouched.
	if isV8SerializedBody(body) {
		if decoded, derr := v8serde.Deserialize([]byte(body)); derr == nil {
			if b, merr := json.Marshal(decoded); merr == nil {
				body = string(b)
			} else {
				return fmt.Errorf("v8 body re-marshal: %w", merr)
			}
		} else {
			return fmt.Errorf("v8 body decode: %w", derr)
		}
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
		"body":        respBody, // []byte -> Uint8Array on the JS side
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
