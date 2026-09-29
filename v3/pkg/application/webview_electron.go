package application

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"runtime"
	"strings"
	"sync"

	"github.com/wailsapp/wails/v3/internal/electron"
)

// platformSetAssetBaseURL is installed by the platform glue (Linux today)
// so the loopback asset server can redirect URL resolution before any
// window resolves its start URL.
var platformSetAssetBaseURL func(scheme, host string)

type electronBackendState struct {
	mu        sync.Mutex
	proc      *electron.Process
	assetsURL string
	windows   map[uint]*electronWindow

	listener net.Listener
	server   *http.Server
}

var electronBackend electronBackendState

func (b *electronBackendState) window(id uint) *electronWindow {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.windows[id]
}

func (b *electronBackendState) setWindow(id uint, w *electronWindow) {
	b.mu.Lock()
	b.windows[id] = w
	b.mu.Unlock()
}

func (b *electronBackendState) dropWindow(id uint) {
	b.mu.Lock()
	delete(b.windows, id)
	b.mu.Unlock()
}

// preparePlatformElectron resolves the Electron runtime and prepares the
// loopback asset server. Called from App.init via the Run path before any
// window exists; no-op unless the electron backend was selected.
func preparePlatformElectron(app *App) error {
	if app.webviewBackend != WebviewBackendElectron {
		return nil
	}
	if runtime.GOOS != "linux" && runtime.GOOS != "windows" {
		return fmt.Errorf("electron backend: %s is not supported yet (linux/windows only)", runtime.GOOS)
	}
	if electronRuntimeProbe != nil {
		if err := electronRuntimeProbe(); err != nil {
			return err
		}
	}

	electronBackend.mu.Lock()
	defer electronBackend.mu.Unlock()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return fmt.Errorf("electron backend: loopback listener: %w", err)
	}
	electronBackend.listener = listener
	_, port, _ := net.SplitHostPort(listener.Addr().String())
	electronBackend.assetsURL = "http://127.0.0.1:" + port
	if platformSetAssetBaseURL != nil {
		platformSetAssetBaseURL("http", "127.0.0.1:"+port)
	}
	electronBackend.server = &http.Server{Handler: app.assets}
	go func() {
		_ = electronBackend.server.Serve(listener)
	}()
	return nil
}

// startPlatformElectron spawns the Electron main process and starts the
// event pump. Called after the platform app exists; no-op unless the
// electron backend was selected.
func startPlatformElectron(app *App) error {
	if app.webviewBackend != WebviewBackendElectron {
		return nil
	}

	electronBackend.mu.Lock()
	defer electronBackend.mu.Unlock()

	exe, err := electron.FindRuntime()
	if err != nil {
		return err
	}
	bootstrap, preload, err := electron.ExtractBootstrap()
	if err != nil {
		return fmt.Errorf("electron backend: extracting bootstrap: %w", err)
	}
	switches := strings.Fields(os.Getenv("WAILS_ELECTRON_SWITCHES"))
	if os.Getuid() == 0 || strings.TrimSpace(os.Getenv("WAILS_ELECTRON_DISABLE_SANDBOX")) == "1" {
		// Chromium's setuid/user namespace sandbox cannot start in typical
		// CI containers; opt out explicitly rather than fail to launch.
		switches = append(switches, "--no-sandbox")
	}
	proc, err := electron.Start(exe, bootstrap, preload, switches, map[string]any{
		"assetsURL": electronBackend.assetsURL,
	})
	if err != nil {
		return err
	}
	electronBackend.proc = proc
	electronBackend.windows = make(map[uint]*electronWindow)

	go pumpElectronEvents(proc)
	return nil
}

// stopPlatformElectron shuts the Electron process and loopback server down.
func stopPlatformElectron() {
	electronBackend.mu.Lock()
	proc := electronBackend.proc
	server := electronBackend.server
	listener := electronBackend.listener
	electronBackend.proc = nil
	electronBackend.mu.Unlock()

	if proc != nil {
		proc.Shutdown()
	}
	if server != nil {
		_ = server.Close()
	}
	if listener != nil {
		_ = listener.Close()
	}
}

func pumpElectronEvents(proc *electron.Process) {
	events := proc.Subscribe()
	for ev := range events {
		switch ev.Name {
		case "message":
			// Renderer postMessage payload: route into the standard message
			// processor exactly like the native script-message handlers do.
			var p struct {
				ID      uint            `json:"id"`
				Payload json.RawMessage `json:"payload"`
			}
			_ = json.Unmarshal(ev.Params, &p)
			msg := string(p.Payload)
			if msg == "" {
				continue
			}
			windowMessageBuffer <- &windowMessage{
				windowId:   p.ID,
				message:    msg,
				originInfo: &OriginInfo{Origin: electronBackend.assetsURL, IsMainFrame: true},
			}
		case "closed":
			if w := electronBackend.window(ev.WindowID); w != nil {
				InvokeSync(func() { w.parent.markAsDestroyed() })
				electronBackend.dropWindow(ev.WindowID)
			}
		case "render-gone":
			if w := electronBackend.window(ev.WindowID); w != nil {
				globalApplication.Logger.Warn("electron renderer gone",
					"window", ev.WindowID, "params", string(ev.Params))
			}
		default:
			if w := electronBackend.window(ev.WindowID); w != nil {
				w.handleEvent(ev)
			}
		}
	}
}
