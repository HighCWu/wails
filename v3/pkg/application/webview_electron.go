package application

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/wailsapp/wails/v3/internal/assetserver/webview"
	"github.com/wailsapp/wails/v3/internal/electron"
)

// platformSetAssetBaseURL is installed by the platform glue (Linux today)
// so the loopback asset server can redirect URL resolution before any
// window resolves its start URL.
var platformSetAssetBaseURL func(scheme, host string)

// platformStartNativeBridge is installed by the platform glue to host the
// per-instance local IPC endpoint (Unix socket on Linux) that the
// renderer's native transport addon dials.
// serveHTTP runs a /wails/runtime request through the asset server and
// returns status, content type and base64 body for the native transport.
var platformStartNativeBridge func(serveHTTP func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error)) (string, string, error)

type electronBackendState struct {
	contextMenus map[uint]*electronContextMenu
	mu        sync.Mutex
	proc      *electron.Process
	assetsURL string
	windows   map[uint]*electronWindow

	listener net.Listener
	server   *http.Server

	// menus persists the menubar *Menu per window id so "menu-click"
	// reports can resolve uids back to menu items
	menus map[uint]*Menu
	// trays persists per-tray state (the menu for uid resolution)
	trays map[uint]*electronTrayState
	// icon is the app icon (data URL) set via App.SetIcon, applied to
	// every live window and to windows created later
	icon string
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
	bridgePath, bridgeToken := "", ""
	nativeAddon := os.Getenv("WAILS_ELECTRON_NATIVE_ADDON")
	// WAILS_ELECTRON_EXPERIMENT is a comma-separated mode list (the preload
	// splits on ','), not a single value.
	experiment := os.Getenv("WAILS_ELECTRON_EXPERIMENT")
	expModes := strings.Split(experiment, ",")
	hasNative := false
	for _, m := range expModes {
		if strings.TrimSpace(m) == "native-ipc" {
			hasNative = true
			break
		}
	}
	if hasNative && nativeAddon != "" && platformStartNativeBridge != nil {
		path, token, err := platformStartNativeBridge(func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error) {
			return serveWebviewRequestDirect(app, method, rawURL, body, hdr)
		})
		if err != nil {
			return err
		}
		bridgePath, bridgeToken = path, token
		app.Logger.Info("electron: native bridge listening", "path", bridgePath)
	} else {
		app.Logger.Info("electron: native bridge skipped", "experiment", experiment,
			"hookInstalled", platformStartNativeBridge != nil)
	}

	proc, err := electron.Start(exe, bootstrap, preload, switches, map[string]any{
		"assetsURL":   electronBackend.assetsURL,
		"bridgePath":  bridgePath,
		"bridgeToken": bridgeToken,
		"nativeAddon": nativeAddon,
	})
	if err != nil {
		return err
	}
	electronBackend.proc = proc
	electronBackend.windows = make(map[uint]*electronWindow)
	electronBackend.contextMenus = make(map[uint]*electronContextMenu)
	electronBackend.menus = make(map[uint]*Menu)
	electronBackend.trays = make(map[uint]*electronTrayState)

	go pumpElectronEvents(proc)
	return nil
}

// serveWebviewRequestDirect runs a /wails/runtime request through the
// asset server in-process. Shared by the control protocol (fetch-ipc)
// and the native UDS transport (native-uds promotion).
func serveWebviewRequestDirect(app *App, method, rawURL, body string, hdr http.Header) (status int, contentType string, bodyRaw []byte, err error) {
	header := http.Header{}
	if hdr != nil {
		for k, vs := range hdr {
			for _, v := range vs {
				header.Add(k, v)
			}
		}
	}
	if u, err := url.Parse(rawURL); err == nil && u.Host != "" {
		header.Set("Host", u.Host)
	}
	var bodyRC io.ReadCloser
	if body != "" {
		bodyRC = io.NopCloser(strings.NewReader(body))
	}

	rw := newStdioResponseWriter()
	req := &stdioWebViewRequest{
		method: method,
		url:    rawURL,
		header: header,
		body:   bodyRC,
		rw:     rw,
	}
	app.assets.ServeWebViewRequest(req)

	select {
	case <-rw.done:
	case <-time.After(15 * time.Second):
		return 0, "", nil, errors.New("webviewRequest timeout")
	}
	return rw.code, rw.header.Get("Content-Type"), rw.buf.Bytes(), nil
}

// stdioWebViewRequest adapts a control-protocol request to the
// assetserver's webview.Request interface.
type stdioWebViewRequest struct {
	method string
	url    string
	header http.Header
	body   io.ReadCloser
	rw     *stdioResponseWriter
}

func (r *stdioWebViewRequest) URL() (string, error)             { return r.url, nil }
func (r *stdioWebViewRequest) Method() (string, error)          { return r.method, nil }
func (r *stdioWebViewRequest) Header() (http.Header, error)     { return r.header, nil }
func (r *stdioWebViewRequest) Body() (io.ReadCloser, error)     { return r.body, nil }
func (r *stdioWebViewRequest) Response() webview.ResponseWriter { return r.rw }
func (r *stdioWebViewRequest) Close() error {
	if r.body != nil {
		return r.body.Close()
	}
	return nil
}

// stdioResponseWriter collects the response produced by the asset server.
type stdioResponseWriter struct {
	header http.Header
	buf    bytes.Buffer
	code   int
	done   chan struct{}
	once   sync.Once
}

func newStdioResponseWriter() *stdioResponseWriter {
	return &stdioResponseWriter{header: make(http.Header), code: 200, done: make(chan struct{})}
}

func (w *stdioResponseWriter) Header() http.Header           { return w.header }
func (w *stdioResponseWriter) Write(buf []byte) (int, error) { return w.buf.Write(buf) }
func (w *stdioResponseWriter) WriteHeader(code int)          { w.code = code }
func (w *stdioResponseWriter) Code() int                     { return w.code }
func (w *stdioResponseWriter) Flush()                        {}
func (w *stdioResponseWriter) Finish() error {
	w.once.Do(func() { close(w.done) })
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
			// The payload is a JSON string (the renderer posts the runtime's
			// invoke payload verbatim); decoding as RawMessage kept the
			// surrounding quotes, so "wails:drag" never matched its prefix.
			var p struct {
				ID      uint   `json:"id"`
				Payload string `json:"payload"`
			}
			_ = json.Unmarshal(ev.Params, &p)
			msg := p.Payload
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
		case "contextmenu-select":
			var p struct {
				ID  uint   `json:"id"`
				UID uint   `json:"uid"`
			}
			_ = json.Unmarshal(ev.Params, &p)
			electronBackend.mu.Lock()
			cm := electronBackend.contextMenus[p.ID]
			electronBackend.mu.Unlock()
			if cm != nil {
				electronBackend.mu.Lock()
				delete(electronBackend.contextMenus, p.ID)
				electronBackend.mu.Unlock()
				InvokeSync(func() { cm.selectUID(p.UID) })
			}
		case "menu-click":
			var p struct {
				ID  uint `json:"id"`
				UID uint `json:"uid"`
			}
			_ = json.Unmarshal(ev.Params, &p)
			electronBackend.mu.Lock()
			menu := electronBackend.menus[p.ID]
			electronBackend.mu.Unlock()
			if menu == nil {
				continue
			}
			if item := findElectronMenuItem(menu.items, p.UID); item != nil {
				InvokeSync(item.handleClick)
			}
		case "tray-menu-click":
			var p struct {
				ID  uint `json:"id"`
				UID uint `json:"uid"`
			}
			_ = json.Unmarshal(ev.Params, &p)
			electronBackend.mu.Lock()
			state := electronBackend.trays[p.ID]
			electronBackend.mu.Unlock()
			if state == nil || state.menu == nil {
				continue
			}
			if item := findElectronMenuItem(state.menu.items, p.UID); item != nil {
				InvokeSync(item.handleClick)
			}
		case "tray-click", "tray-right-click", "tray-double-click",
			"tray-right-double-click", "tray-mouse-enter", "tray-mouse-leave":
			var p struct {
				ID uint `json:"id"`
			}
			_ = json.Unmarshal(ev.Params, &p)
			electronBackend.mu.Lock()
			st := electronBackend.trays[p.ID]
			electronBackend.mu.Unlock()
			if st == nil {
				continue
			}
			tray := st.parent
			InvokeSync(func() {
				switch ev.Name {
				case "tray-click":
					if tray.clickHandler != nil {
						tray.clickHandler()
					}
				case "tray-right-click":
					if tray.rightClickHandler != nil {
						tray.rightClickHandler()
					}
				case "tray-double-click":
					if tray.doubleClickHandler != nil {
						tray.doubleClickHandler()
					}
				case "tray-right-double-click":
					if tray.rightDoubleClickHandler != nil {
						tray.rightDoubleClickHandler()
					}
				case "tray-mouse-enter":
					if tray.mouseEnterHandler != nil {
						tray.mouseEnterHandler()
					}
				case "tray-mouse-leave":
					if tray.mouseLeaveHandler != nil {
						tray.mouseLeaveHandler()
					}
				}
			})
		case "render-gone":
			if w := electronBackend.window(ev.WindowID); w != nil {
				var p struct {
					Reason string `json:"reason"`
				}
				_ = json.Unmarshal(ev.Params, &p)
				w.handleRendererGone(p.Reason)
			}
		default:
			if w := electronBackend.window(ev.WindowID); w != nil {
				w.handleEvent(ev)
			}
		}
	}
}

// electronAcceleratorString canonicalizes a before-input-event key report
// into the exact acc.String() form the binding registries are keyed by
// (modifiers sorted, key uppercased). DOM key names are mapped to the
// wails named keys; unparseable keys (media keys, dead keys) are dropped.
func electronAcceleratorString(key string, alt, ctrl, shift, meta bool) (string, bool) {
	if key == "" {
		return "", false
	}
	if mapped, ok := electronDOMKeyMap[strings.ToLower(key)]; ok {
		key = mapped
	}
	parts := make([]string, 0, 5)
	if shift {
		parts = append(parts, "shift")
	}
	if ctrl {
		parts = append(parts, "ctrl")
	}
	if alt {
		parts = append(parts, "alt")
	}
	if meta {
		parts = append(parts, "super")
	}
	parts = append(parts, key)
	acc, err := parseAccelerator(strings.Join(parts, "+"))
	if err != nil {
		return "", false
	}
	return acc.String(), true
}

var electronDOMKeyMap = map[string]string{
	"escape":     "escape",
	"enter":      "enter",
	"return":     "return",
	"tab":        "tab",
	"backspace":  "backspace",
	"delete":     "delete",
	"arrowup":    "up",
	"arrowdown":  "down",
	"arrowleft":  "left",
	"arrowright": "right",
	" ":          "space",
	"pageup":     "page up",
	"pagedown":   "page down",
}

// findElectronMenuItem resolves a serialized uid back to its menu item.
func findElectronMenuItem(items []*MenuItem, uid uint) *MenuItem {
	for _, it := range items {
		if it.id == uid {
			return it
		}
		if it.submenu != nil {
			if found := findElectronMenuItem(it.submenu.items, uid); found != nil {
				return found
			}
		}
	}
	return nil
}

// electronSetApplicationMenu is the electron branch of the platform
// setApplicationMenu (hooked from the linuxApp/windowsApp methods):
// id 0 routes to Menu.setApplicationMenu in main.js.
func electronSetApplicationMenu(menu *Menu) {
	electronBackend.mu.Lock()
	electronBackend.menus[0] = menu
	electronBackend.mu.Unlock()
	if menu == nil {
		return
	}
	// the spec rides any live window's control channel (id 0 selects the
	// application menu in main.js); with no window up there is nothing to
	// attach a menu to yet
	for _, w := range electronWindowsSnapshot() {
		_ = w.call("setMenu", map[string]any{
			"id":   uint(0),
			"menu": electronSerializeMenu(menu.items),
		})
		break
	}
}

// electronSetIcon is the electron branch of the platform setIcon: apply to
// every live window now and remember it for windows created later.
func electronSetIcon(icon []byte) {
	dataURL := "data:image/png;base64," + base64.StdEncoding.EncodeToString(icon)
	electronBackend.mu.Lock()
	electronBackend.icon = dataURL
	electronBackend.mu.Unlock()
	for _, w := range electronWindowsSnapshot() {
		_ = w.call("setIcon", map[string]any{"icon": dataURL})
	}
}

func electronWindowsSnapshot() []*electronWindow {
	electronBackend.mu.Lock()
	defer electronBackend.mu.Unlock()
	out := make([]*electronWindow, 0, len(electronBackend.windows))
	for _, w := range electronBackend.windows {
		out = append(out, w)
	}
	return out
}
