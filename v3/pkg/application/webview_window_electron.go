package application

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"sync"
	"time"
	"unsafe"

	"github.com/wailsapp/wails/v3/internal/assetserver"
	"github.com/wailsapp/wails/v3/internal/debounce"
	"github.com/wailsapp/wails/v3/internal/electron"
	"github.com/wailsapp/wails/v3/pkg/events"
)

// electronWindow is the webviewWindowImpl for the electron backend: the
// window lives in the Electron main process and every operation is a
// control-protocol call. Geometry and interaction state are cached from
// window events so hit-testing loops never round-trip the IPC channel.
type electronWindow struct {
	parent *WebviewWindow

	mu           sync.Mutex
	x, y         int
	curWidth     int
	curHeight    int
	title        string
	visible      bool
	focused      bool
	minimised    bool
	maximised    bool
	inFullscreen bool
	ignoring     bool
	zoomLevel    float64

	// crash recovery: one auto-reload per renderer death, with a cooldown
	// so a page that crashes on every load cannot turn into a reload loop
	lastCrashReload time.Time

	// interactive resize/move fire per-frame on the Electron side; the
	// WindowDidResize/WindowDidMove events go out debounced like the GTK
	// backend's (options.Linux.WindowDidMoveDebounceMS, 50ms default)
	moveDebouncer   func(func())
	resizeDebouncer func(func())
}

// set from WAILS_ELECTRON_DEBUG=1 at backend start; window state events
// are the only observability into what the Electron process did.
var debugElectronWindowEvents = os.Getenv("WAILS_ELECTRON_DEBUG") == "1"

func newElectronWindow(parent *WebviewWindow) *electronWindow {
	return &electronWindow{parent: parent, zoomLevel: 1.0, visible: true}
}

// handleRendererGone recovers the page after an abnormal renderer death:
// report via a plain custom event (the upstream event registry has no
// typed renderer-crash event, and the events package stays byte-identical
// to upstream to keep merges conflict-free), then schedule exactly one
// auto-reload — a 10s cooldown guards reload loops. clean-exit is a
// normal shutdown, not a crash.
func (w *electronWindow) handleRendererGone(reason string) {
	if reason == "" || reason == "clean-exit" {
		return
	}
	w.mu.Lock()
	cooling := time.Since(w.lastCrashReload) < 10*time.Second
	w.lastCrashReload = time.Now()
	w.mu.Unlock()
	globalApplication.Logger.Warn("electron renderer crashed",
		"window", w.parent.ID(), "reason", reason, "cooldown", cooling)
	globalApplication.Event.Emit("electron:rendererCrashed", map[string]any{
		"id": w.parent.ID(), "reason": reason,
	})
	if !cooling {
		// let the Electron main process finish tearing the dead
		// webContents down before the reload lands
		time.AfterFunc(200*time.Millisecond, func() { w.parent.Reload() })
	}
}

func (w *electronWindow) proc() (*electron.Process, error) {
	electronBackend.mu.Lock()
	defer electronBackend.mu.Unlock()
	if electronBackend.proc == nil {
		return nil, errors.New("electron backend is not running")
	}
	return electronBackend.proc, nil
}

func (w *electronWindow) call(method string, params map[string]any) error {
	proc, err := w.proc()
	if err != nil {
		return err
	}
	return proc.Call(method, w.parent.ID(), params, nil)
}

func (w *electronWindow) callBounds(method string, params map[string]any) (x, y, width, height int, err error) {
	proc, err := w.proc()
	if err != nil {
		return 0, 0, 0, 0, err
	}
	var b struct {
		X      int `json:"x"`
		Y      int `json:"y"`
		Width  int `json:"width"`
		Height int `json:"height"`
	}
	if err := proc.Call(method, w.parent.ID(), params, &b); err != nil {
		return 0, 0, 0, 0, err
	}
	return b.X, b.Y, b.Width, b.Height, nil
}

// run creates the BrowserWindow. Like the GTK path, it returns as soon as
// the window exists — the Electron process keeps everything alive.
func (w *electronWindow) run() {
	o := w.parent.options
	startURL, err := assetserver.GetStartURL(o.URL)
	if err != nil {
		globalApplication.Logger.Error("electron: resolving start URL", "error", err)
		return
	}
	x, y, width, height, err := w.callBounds("create", map[string]any{
		"x":           o.X,
		"y":           o.Y,
		"width":       o.Width,
		"height":      o.Height,
		"title":       o.Title,
		"frameless":   o.Frameless,
		"transparent": o.BackgroundType != BackgroundTypeSolid,
		"resizable":   !o.DisableResize,
		"alwaysOnTop": o.AlwaysOnTop,
		"url":         startURL,

		"enableFileDrop": o.EnableFileDrop,
	})
	if err != nil {
		globalApplication.Logger.Error("electron: window create failed", "window", w.parent.ID(), "error", err)
		return
	}
	w.mu.Lock()
	w.x, w.y, w.curWidth, w.curHeight = x, y, width, height
	w.mu.Unlock()
	if w.moveDebouncer == nil {
		debounceMS := o.Linux.WindowDidMoveDebounceMS
		if debounceMS == 0 {
			debounceMS = 50
		}
		w.moveDebouncer = debounce.New(time.Duration(debounceMS) * time.Millisecond)
		w.resizeDebouncer = debounce.New(time.Duration(debounceMS) * time.Millisecond)
	}
	electronBackend.setWindow(w.parent.ID(), w)
}

// handleEvent applies window state events from the Electron process.
func (w *electronWindow) handleEvent(ev electron.Event) {
	var b struct {
		X      int `json:"x"`
		Y      int `json:"y"`
		Width  int `json:"width"`
		Height int `json:"height"`
	}
	_ = json.Unmarshal(ev.Params, &b)
	w.mu.Lock()
	defer w.mu.Unlock()
	if debugElectronWindowEvents {
		globalApplication.Logger.Debug("electron window event",
			"window", w.parent.ID(), "event", ev.Name)
	}
	switch ev.Name {
	case "resize":
		if b.Width > 0 && b.Height > 0 {
			w.x, w.y, w.curWidth, w.curHeight = b.X, b.Y, b.Width, b.Height
		}
		// the GTK backend debounces these (50ms default); without it an
		// interactive resize floods the windowEvents channel
		if w.resizeDebouncer != nil {
			w.resizeDebouncer(func() { w.parent.emit(events.Common.WindowDidResize) })
		}
	case "move":
		if b.Width > 0 && b.Height > 0 {
			w.x, w.y, w.curWidth, w.curHeight = b.X, b.Y, b.Width, b.Height
		}
		if w.moveDebouncer != nil {
			w.moveDebouncer(func() { w.parent.emit(events.Common.WindowDidMove) })
		}
	case "focus":
		w.focused = true
		w.parent.emit(events.Common.WindowFocus)
	case "blur":
		w.focused = false
		w.parent.emit(events.Common.WindowLostFocus)
	case "show":
		w.visible = true
		w.parent.emit(events.Common.WindowShow)
	case "hide":
		w.visible = false
		w.parent.emit(events.Common.WindowHide)
	case "minimise":
		w.minimised = true
		w.parent.emit(events.Common.WindowMinimise)
	case "restore":
		w.minimised = false
		w.parent.emit(events.Common.WindowUnMinimise)
	case "maximise":
		w.maximised = true
		w.parent.emit(events.Common.WindowMaximise)
	case "unmaximise":
		w.maximised = false
		w.parent.emit(events.Common.WindowUnMaximise)
	case "fullscreen":
		w.inFullscreen = true
		w.parent.emit(events.Common.WindowFullscreen)
	case "unfullscreen":
		w.inFullscreen = false
		w.parent.emit(events.Common.WindowUnFullscreen)
	}
}

// real implementations -------------------------------------------------------

func (w *electronWindow) setTitle(title string) {
	w.mu.Lock()
	w.title = title
	w.mu.Unlock()
	_ = w.call("setTitle", map[string]any{"title": title})
}

func (w *electronWindow) setSize(width, height int) {
	_ = w.call("setSize", map[string]any{"width": width, "height": height})
}

func (w *electronWindow) setAlwaysOnTop(alwaysOnTop bool) {
	_ = w.call("setAlwaysOnTop", map[string]any{"v": alwaysOnTop})
}

func (w *electronWindow) setURL(url string) {}

func (w *electronWindow) setResizable(resizable bool) {
	_ = w.call("setResizable", map[string]any{"v": resizable})
	// mirror the runtime flag (GTK parity): the page's frameless
	// edge-resize detection stays disabled until this is set
	w.execJS(fmt.Sprintf("if(window._wails&&window._wails.setResizable)window._wails.setResizable(%v);", resizable))
}

func (w *electronWindow) setMinSize(width, height int) {
	_ = w.call("setMinimumSize", map[string]any{"width": width, "height": height})
}

func (w *electronWindow) setMaxSize(width, height int) {
	_ = w.call("setMaximumSize", map[string]any{"width": width, "height": height})
}

func (w *electronWindow) execJS(js string) {
	_ = w.call("execJS", map[string]any{"js": js})
}

func (w *electronWindow) setBackgroundColour(color RGBA) {
	_ = w.call("setBackgroundColour", map[string]any{
		"colour": fmt.Sprintf("#%02x%02x%02x%02x", color.Red, color.Green, color.Blue, color.Alpha),
	})
}

func (w *electronWindow) center() { _ = w.call("center", nil) }

func (w *electronWindow) size() (int, int) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.curWidth, w.curHeight
}

func (w *electronWindow) width() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.curWidth
}

func (w *electronWindow) height() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.curHeight
}

func (w *electronWindow) destroy() {
	_ = w.call("destroy", nil)
	w.parent.markAsDestroyed()
}

func (w *electronWindow) reload()             { _ = w.call("reload", nil) }
func (w *electronWindow) forceReload()        { _ = w.call("forceReload", nil) }
func (w *electronWindow) openDevTools()       { _ = w.call("openDevTools", nil) }
func (w *electronWindow) zoomReset()          { w.setZoom(1.0) }
func (w *electronWindow) zoomIn()             { w.mu.Lock(); z := w.zoomLevel * 1.1; w.mu.Unlock(); w.setZoom(z) }
func (w *electronWindow) zoomOut()            { w.mu.Lock(); z := w.zoomLevel / 1.1; w.mu.Unlock(); w.setZoom(z) }
func (w *electronWindow) zoom()               {}
func (w *electronWindow) setHTML(html string) {}

func (w *electronWindow) getZoom() float64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.zoomLevel
}

func (w *electronWindow) setZoom(zoom float64) {
	w.mu.Lock()
	w.zoomLevel = zoom
	w.mu.Unlock()
	_ = w.call("setZoom", map[string]any{"v": zoom})
}

func (w *electronWindow) close() { _ = w.call("close", nil) }

func (w *electronWindow) on(eventID uint) {}

func (w *electronWindow) minimise()   { _ = w.call("minimise", nil) }
func (w *electronWindow) unminimise() { _ = w.call("unminimise", nil) }
func (w *electronWindow) maximise()   { _ = w.call("maximise", nil) }
func (w *electronWindow) unmaximise() { _ = w.call("unmaximise", nil) }
func (w *electronWindow) fullscreen() { _ = w.call("setFullScreen", map[string]any{"v": true}) }
func (w *electronWindow) unfullscreen() {
	_ = w.call("setFullScreen", map[string]any{"v": false})
}

func (w *electronWindow) isMinimised() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.minimised
}

func (w *electronWindow) isMaximised() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.maximised
}

func (w *electronWindow) isFullscreen() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.inFullscreen
}

func (w *electronWindow) isNormal() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return !w.minimised && !w.maximised && !w.inFullscreen
}

func (w *electronWindow) isVisible() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.visible
}

func (w *electronWindow) isFocused() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.focused
}

func (w *electronWindow) focus() { _ = w.call("focus", nil) }
func (w *electronWindow) show()  { _ = w.call("show", nil) }
func (w *electronWindow) hide()  { _ = w.call("hide", nil) }

func (w *electronWindow) getScreen() (*Screen, error) {
	return nil, errors.New("screens not available on the electron backend yet")
}

func (w *electronWindow) setFrameless(frameless bool) {}

func (w *electronWindow) openContextMenu(menu *Menu, data *ContextMenuData) {}

func (w *electronWindow) nativeWindow() unsafe.Pointer { return nil }

// startDrag/startResize run the frameless window drag/resize through the
// control protocol: the addon sends _NET_WM_MOVERESIZE to the X server so
// the window manager conducts the move/resize interactively (GTK parity).
func (w *electronWindow) startDrag() error {
	return w.call("startDrag", nil)
}

func (w *electronWindow) startResize(border string) error {
	return w.call("startResize", map[string]any{"edge": border})
}

func (w *electronWindow) print() error {
	return errors.New("print not available on the electron backend yet")
}

func (w *electronWindow) setEnabled(enabled bool) {}

func (w *electronWindow) physicalBounds() Rect {
	w.mu.Lock()
	defer w.mu.Unlock()
	return Rect{X: w.x, Y: w.y, Width: w.curWidth, Height: w.curHeight}
}

func (w *electronWindow) setPhysicalBounds(physicalBounds Rect) {
	w.setBounds(physicalBounds)
}

func (w *electronWindow) bounds() Rect {
	w.mu.Lock()
	defer w.mu.Unlock()
	return Rect{X: w.x, Y: w.y, Width: w.curWidth, Height: w.curHeight}
}

func (w *electronWindow) setBounds(bounds Rect) {
	_ = w.call("setBounds", map[string]any{
		"x": bounds.X, "y": bounds.Y, "width": bounds.Width, "height": bounds.Height,
	})
	w.mu.Lock()
	w.x, w.y, w.curWidth, w.curHeight = bounds.X, bounds.Y, bounds.Width, bounds.Height
	w.mu.Unlock()
}

func (w *electronWindow) position() (int, int) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.x, w.y
}

func (w *electronWindow) setPosition(x, y int) {
	_ = w.call("setPosition", map[string]any{"x": x, "y": y})
	w.mu.Lock()
	w.x, w.y = x, y
	w.mu.Unlock()
}

func (w *electronWindow) centerOnScreen(screen *Screen) { w.center() }

func (w *electronWindow) relativePosition() (int, int) { return w.position() }

func (w *electronWindow) setRelativePosition(x, y int) { w.setPosition(x, y) }

func (w *electronWindow) flash(enabled bool)                      {}
func (w *electronWindow) handleKeyEvent(acceleratorString string) {}

func (w *electronWindow) getBorderSizes() *LRTB { return &LRTB{} }

func (w *electronWindow) setMinimiseButtonState(state ButtonState)   {}
func (w *electronWindow) setMaximiseButtonState(state ButtonState)   {}
func (w *electronWindow) setCloseButtonState(state ButtonState)      {}
func (w *electronWindow) setFullscreenButtonState(state ButtonState) {}

func (w *electronWindow) isIgnoreMouseEvents() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.ignoring
}

func (w *electronWindow) setIgnoreMouseEvents(ignore bool) {
	w.mu.Lock()
	if w.ignoring == ignore {
		w.mu.Unlock()
		return
	}
	w.ignoring = ignore
	w.mu.Unlock()
	_ = w.call("setIgnoreMouseEvents", map[string]any{"v": ignore})
}

func (w *electronWindow) cut()    { _ = w.call("cut", nil) }
func (w *electronWindow) copy()   { _ = w.call("copy", nil) }
func (w *electronWindow) paste()  { _ = w.call("paste", nil) }
func (w *electronWindow) undo()   { _ = w.call("undo", nil) }
func (w *electronWindow) delete() { _ = w.call("delete", nil) }
func (w *electronWindow) selectAll() {
	_ = w.call("selectAll", nil)
}
func (w *electronWindow) redo() { _ = w.call("redo", nil) }

func (w *electronWindow) showMenuBar()       {}
func (w *electronWindow) hideMenuBar()       {}
func (w *electronWindow) toggleMenuBar()     {}
func (w *electronWindow) setMenu(menu *Menu) {}

func (w *electronWindow) snapAssist() {}

// attachModal links the modal window to this parent via Electron's
// setParentWindow (Electron's modal flag is construction-time, so this is
// parent-linking rather than true modality on Linux).
func (w *electronWindow) attachModal(modalWindow *WebviewWindow) {
	if mw, ok := modalWindow.impl.(*electronWindow); ok {
		_ = mw.call("setParent", map[string]any{"parent": w.parent.ID()})
	}
}

func (w *electronWindow) setContentProtection(enabled bool) {}

func (w *electronWindow) setNonClientHitTestRegions(regions []nonClientHitTestRegion) {}
