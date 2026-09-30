//go:build linux

package application

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
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
	sc := bufio.NewScanner(conn)
	sc.Buffer(make([]byte, 0, 4096), 16*1024*1024)
	// HELLO handshake: the first frame must carry the per-instance token;
	// anything else is a foreign local process and gets dropped silently.
	if !sc.Scan() {
		return
	}
	var hello struct {
		Hello int    `json:"hello"`
		Token string `json:"token"`
	}
	if err := json.Unmarshal(sc.Bytes(), &hello); err != nil ||
		hello.Hello != 1 || hello.Token != expectedToken {
		return
	}
	conn.Write([]byte(`{"ready":true}` + "\n"))
	for sc.Scan() {
		line := sc.Bytes()
		if len(line) == 0 {
			continue
		}
		var frame struct {
			ID      int    `json:"id"`
			Payload string `json:"payload"`
		}
		if err := json.Unmarshal(line, &frame); err != nil {
			continue
		}
		resp, _ := json.Marshal(frame)
		conn.Write(append(resp, '\n'))
	}
}

func dirForBridgeSocket() (string, error) {
	if runtime := os.Getenv("XDG_RUNTIME_DIR"); runtime != "" {
		return runtime, nil
	}
	return os.TempDir(), nil
}
