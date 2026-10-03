//go:build darwin

package application

// Darwin half of the electron backend's native transport: the bridge
// endpoint is a per-instance Unix socket under XDG_RUNTIME_DIR (or /tmp),
// identical to the linux transport.

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

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

	serveBridgeListener(ln, tokenHex, serveHTTP)
	return path, tokenHex, nil
}

func dirForBridgeSocket() (string, error) {
	if runtime := os.Getenv("XDG_RUNTIME_DIR"); runtime != "" {
		return runtime, nil
	}
	return os.TempDir(), nil
}
