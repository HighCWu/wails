//go:build windows

package application

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net/http"
	"os"
	"time"

	"github.com/Microsoft/go-winio"
	"github.com/wailsapp/wails/v3/internal/assetserver"
)

func init() {
	// The electron backend serves the asset server from a loopback HTTP
	// listener; redirect URL resolution before any window resolves its
	// start URL (mirrors the CEF backend's http://wails.localhost choice).
	platformSetAssetBaseURL = assetserver.SetBaseURL
	// Local IPC endpoint for the renderer's native transport addon: a
	// per-instance named pipe (the Windows counterpart of the Unix
	// socket used on linux) carrying the same protocol v3 frames.
	platformStartNativeBridge = startNativeBridgeListener
}

func startNativeBridgeListener(serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) (string, string, error) {
	token := make([]byte, 16)
	if _, err := rand.Read(token); err != nil {
		return "", "", err
	}
	tokenHex := hex.EncodeToString(token)
	pipe := fmt.Sprintf(`\\.\pipe\wails-electron-%d-%d`, os.Getpid(), time.Now().UnixNano())
	ln, err := winio.ListenPipe(pipe, nil)
	if err != nil {
		return "", "", err
	}
	nativeBridge.mu.Lock()
	nativeBridge.path = pipe
	nativeBridge.ln = ln
	nativeBridge.mu.Unlock()

	serveBridgeListener(ln, tokenHex, serveHTTP)
	return pipe, tokenHex, nil
}
