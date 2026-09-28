//go:build linux && cgo && wails_cef && !android && !server

package application

/*
#cgo gtk3 pkg-config: gtk+-3.0 gdk-3.0 x11
#cgo gtk3 CFLAGS: -DWAILS_CEF_GTK3
#cgo !gtk3 pkg-config: gtk4 x11
#ifdef WAILS_CEF_GTK3
#include "cef_widget_gtk3.h"
#else
#include "cef_widget_gtk4.h"
#endif
*/
import "C"

import (
	"fmt"
	"strings"
	"sync"
	"unsafe"

	"github.com/wailsapp/wails/v3/internal/cef"
)

// linuxCEFWebview is the CEF flavour of the in-window webview engine: a
// plain GTK drawing area whose X11 window hosts CEF's native browser
// window. All webview-facing linuxWebviewWindow methods dispatch here
// when w.cefEngine != nil.
type linuxCEFWebview struct {
	nativeClosed bool
	parent       *linuxWebviewWindow
	widget       pointer // GtkDrawingArea (owns the X11 host window)
	windowID     uint

	// background (0xAARRGGBB, 0 = CEF default) applied at browser
	// creation; CEF latches it with the first paint.
	background uint32

	closed                         bool
	moving                         bool
	moveX, moveY, windowX, windowY int
	windowWidth, windowHeight      int
	resizeEdge                     string
	lastDragX, lastDragY           int

	// creating guards concurrent attach attempts; attached means the
	// browser exists (and is only set on success so lazy retries work).
	creating bool

	mu         sync.Mutex
	browser    *cef.Browser
	startURL   string
	attached   bool
	attachErr  error
	lastWidth  int
	lastHeight int
}

// cefEngines maps the GTK widget pointer to its engine so the C callback
// trampolines can find the Go side.
var cefEngines sync.Map // unsafe.Pointer(widget) -> *linuxCEFWebview

func init() {
	createWindowWebview = newCEFWebview
	cefEngineForWidget = func(webview pointer) cefEngineHooks {
		if e, ok := cefEngines.Load(unsafe.Pointer(webview)); ok {
			return e.(*linuxCEFWebview)
		}
		return nil
	}
}

// newCEFWebview creates the GTK container widget used in place of a
// WebKitWebView. The CEF browser itself is created when the widget is
// mapped (its X11 window only exists then).
func newCEFWebview(windowId uint, gpuPolicy WebviewGpuPolicy) pointer {
	if globalApplication.webviewBackend != WebviewBackendCEF {
		return windowNewWebview(windowId, gpuPolicy)
	}
	widget := pointer(C.gtk_drawing_area_new())
	C.wails_cef_set_default_visual((*C.GtkWidget)(widget))
	C.wails_cef_enable_dnd((*C.GtkWidget)(widget), C.uintptr_t(windowId))
	C.gtk_widget_set_can_focus((*C.GtkWidget)(widget), C.gboolean(1))
	e := &linuxCEFWebview{widget: widget, windowID: windowId}
	cefEngines.Store(unsafe.Pointer(widget), e)
	C.wails_cef_connect_map((*C.GtkWidget)(widget), nil)
	C.wails_cef_connect_size_allocate((*C.GtkWidget)(widget), nil)
	return widget
}

// tryAttach attaches lazily once the X window exists and the initial URL
// has arrived; called from the 10ms GTK pump tick, which is more reliable
// than widget signals (size-allocate can fire pre-realize and map can be
// swallowed entirely).
func (e *linuxCEFWebview) tryAttach() {
	e.mu.Lock()
	done := e.closed || e.attached || e.creating || e.startURL == "" || e.browser != nil
	e.mu.Unlock()
	if done {
		return
	}
	if uintptr(C.wails_cef_widget_xid((*C.GtkWidget)(e.widget))) == 0 {
		return
	}
	e.attach()
}

// attach creates the CEF browser inside the widget's X11 window. Called
// on the GTK main thread from the map handler.
func (e *linuxCEFWebview) attach() {
	e.mu.Lock()
	if e.closed || e.attached || e.creating {
		e.mu.Unlock()
		return
	}
	e.creating = true
	e.mu.Unlock()
	defer func() {
		e.mu.Lock()
		e.creating = false
		e.mu.Unlock()
	}()

	if e.startURL == "" {
		// The window's initial URL arrives via setURL after the first
		// allocation; defer browser creation until then (loadURL calls
		// attach).
		return
	}
	xid := uintptr(C.wails_cef_widget_xid((*C.GtkWidget)(e.widget)))
	if xid == 0 {
		e.attachErr = fmt.Errorf("CEF: widget has no X11 window")
		return
	}

	winWidth, winHeight := e.parent.size()
	browser, err := cef.CreateBrowser(cef.CreateBrowserOptions{
		WindowID:        e.windowID,
		ParentXWindow:   xid,
		URL:             e.startURL,
		Width:           winWidth,
		Height:          winHeight,
		BackgroundColor: e.background,
	})
	if err != nil {
		e.attachErr = err
		globalApplication.handleFatalError(fmt.Errorf("CEF backend: %w", err))
		return
	}
	e.mu.Lock()
	e.browser = browser
	e.attached = true
	e.mu.Unlock()

	// The GTK window is already visible at this point; size the browser to
	// the current allocation in case it arrived before the browser did.
	var cw, ch C.int
	C.wails_cef_widget_size((*C.GtkWidget)(e.widget), &cw, &ch)
	if cw > 1 && ch > 1 {
		e.resizeBrowser(int(cw), int(ch))
	}
}

// setBackgroundColour stores the base colour (RGBA) applied when the
// browser is created.
func (e *linuxCEFWebview) setBackgroundColour(colour RGBA) {
	e.background = uint32(colour.Alpha)<<24 | uint32(colour.Red)<<16 |
		uint32(colour.Green)<<8 | uint32(colour.Blue)
}

// openDevTools opens CEF's devtools window for the embedded browser.
func (e *linuxCEFWebview) openDevTools() {
	if b := e.browserOrWait(); b != nil {
		b.OpenDevTools()
	}
}

// focusBrowser moves the X keyboard focus onto the browser's native
// window; called from the GTK toplevel's focus-in-event.
func (e *linuxCEFWebview) focusBrowser() {
	if b := e.browserOrWait(); b != nil && b.XWindow() != 0 {
		b.Focus()
		C.wails_cef_focus_browser(C.ulong(b.XWindow()))
	}
}

// queryPointerDragState returns (button, rootX, rootY) of the pointer
// for drag synthesis; button 0 means no button is held.
func (e *linuxCEFWebview) queryPointerDragState() (int, int, int) {
	var btn, x, y C.int
	if C.wails_cef_query_pointer((*C.GtkWidget)(e.widget), &btn, &x, &y) != 1 {
		return 0, 0, 0
	}
	return int(btn), int(x), int(y)
}

// setStartURL records the URL to load when the browser is created (or
// navigates immediately if it already exists).
func (e *linuxCEFWebview) setStartURL(url string) {
	e.startURL = url
	e.mu.Lock()
	browser := e.browser
	attached := e.attached
	e.mu.Unlock()
	if browser != nil {
		e.execJS(fmt.Sprintf("window.location.href = %q;", url))
		return
	}
	if !attached {
		// First URL: drive browser creation now that the widget is laid
		// out (attach is a no-op until startURL is set).
		e.attach()
	}
}

func (e *linuxCEFWebview) browserOrWait() *cef.Browser {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.browser
}

func (e *linuxCEFWebview) execJS(js string) {
	if b := e.browserOrWait(); b != nil {
		b.ExecJS(js)
	}
}

func (e *linuxCEFWebview) loadURL(url string) {
	e.mu.Lock()
	browser := e.browser
	e.mu.Unlock()
	if browser == nil {
		e.setStartURL(url)
		return
	}
	browser.LoadURL(url)
}

func (e *linuxCEFWebview) reload(ignoreCache bool) {
	if b := e.browserOrWait(); b != nil {
		b.Reload(ignoreCache)
	}
}

func (e *linuxCEFWebview) stopAndClose() {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return
	}
	e.closed = true
	browser := e.browser
	e.mu.Unlock()
	if browser != nil {
		browser.Close(true)
	} else {
		e.finishClose()
	}
}

// CEF must destroy its own X11 child before GTK destroys the parent.
// OnBeforeClose schedules this after DevTools detach and unload complete.
func (e *linuxCEFWebview) finishClose() {
	if e.nativeClosed {
		return
	}
	e.nativeClosed = true
	cefEngines.Delete(unsafe.Pointer(e.widget))
	C.gtk_widget_destroy((*C.GtkWidget)(e.parent.window))
	getNativeApplication().unregisterWindow(windowPointer(e.parent.window))
}

func finishCEFWindowClose(windowID uint) {
	cefEngines.Range(func(_, value any) bool {
		engine := value.(*linuxCEFWebview)
		if engine.windowID == windowID {
			engine.finishClose()
			return false
		}
		return true
	})
}

// resizeBrowser sizes the native browser child to match the GTK container.
// Creation is asynchronous: retry from the pump until the native handle exists.
func (e *linuxCEFWebview) resizeBrowser(width, height int) {
	if width <= 1 || height <= 1 {
		return
	}
	b := e.browserOrWait()
	if b == nil {
		return
	}
	xid := b.XWindow()
	if xid == 0 {
		return
	}
	e.mu.Lock()
	if e.lastWidth == width && e.lastHeight == height {
		e.mu.Unlock()
		return
	}
	e.lastWidth, e.lastHeight = width, height
	e.mu.Unlock()
	C.wails_cef_resize_browser(C.ulong(xid), C.int(width), C.int(height))

}

func (e *linuxCEFWebview) syncSize() {
	if e.closed {
		return
	}
	e.syncMove()
	var width, height C.int
	C.wails_cef_widget_size((*C.GtkWidget)(e.widget), &width, &height)
	e.resizeBrowser(int(width), int(height))
	if C.wails_cef_toplevel_has_xfocus((*C.GtkWidget)(e.widget)) != 0 {
		e.focusBrowser()
	}
}

// setZoomLevel maps the wails zoom factor (1.0 = 100%) onto CEF's
// logarithmic zoom level.
func (e *linuxCEFWebview) setZoomFactor(zoom float64) {
	if zoom < 1 {
		zoom = 1
	}
	if b := e.browserOrWait(); b != nil {
		b.SetZoomFactor(zoom)
	}
}

func (e *linuxCEFWebview) zoomFactor() float64 {
	if b := e.browserOrWait(); b != nil {
		return b.ZoomFactor()
	}
	return 1
}

// engineForToplevel resolves the engine whose parent GTK toplevel is the
// given window pointer.
func engineForToplevel(toplevel unsafe.Pointer) *linuxCEFWebview {
	var found *linuxCEFWebview
	cefEngines.Range(func(_, v any) bool {
		e := v.(*linuxCEFWebview)
		if e.parent != nil && unsafe.Pointer(e.parent.window) == toplevel {
			found = e
			return false
		}
		return true
	})
	return found
}

// attachCEFEngine links the engine created by windowNewCEFWebview to its
// window. Called from run() right after windowNew.
func (w *linuxWebviewWindow) attachCEFEngine() {
	if e, ok := cefEngines.Load(unsafe.Pointer(w.webview)); ok {
		engine := e.(*linuxCEFWebview)
		C.wails_cef_set_default_visual((*C.GtkWidget)(w.window))
		engine.parent = w
		C.wails_cef_connect_focus((*C.GtkWidget)(w.window))
		w.cefEngine = engine
	}
}

var _ cefEngineHooks = (*linuxCEFWebview)(nil)

// getLinuxEngine returns the CEF engine of a window, or nil.
func getLinuxEngine(window Window) *linuxCEFWebview {
	impl := getLinuxWebviewWindow(window)
	if impl == nil || impl.cefEngine == nil {
		return nil
	}
	engine, _ := impl.cefEngine.(*linuxCEFWebview)
	return engine
}

// Chromium owns the pointer grab on its native child. GTK/WM move requests
// cannot acquire that grab, so follow root coordinates while the button is held.
// This also keeps working when the pointer leaves the embedded child.
func (e *linuxCEFWebview) beginWindowDrag(edge string) {
	if e.moving {
		return
	}
	button, x, y := e.queryPointerDragState()
	if button != 1 {
		return
	}
	var wx, wy, width, height C.int
	C.gtk_window_get_position((*C.GtkWindow)(e.parent.window), &wx, &wy)
	C.gtk_window_get_size((*C.GtkWindow)(e.parent.window), &width, &height)
	e.windowWidth, e.windowHeight = int(width), int(height)
	e.resizeEdge = strings.TrimSuffix(edge, "-resize")
	e.lastDragX, e.lastDragY = x, y
	e.moving = true
	e.moveX, e.moveY = x, y
	e.windowX, e.windowY = int(wx), int(wy)
}

func (e *linuxCEFWebview) syncMove() {
	if !e.moving {
		return
	}
	button, x, y := e.queryPointerDragState()
	if button != 1 {
		e.moving = false
		return
	}
	if x == e.lastDragX && y == e.lastDragY {
		return
	}
	e.lastDragX, e.lastDragY = x, y
	scale := max(1, int(C.gtk_widget_get_scale_factor((*C.GtkWidget)(e.widget))))
	dx, dy := (x-e.moveX)/scale, (y-e.moveY)/scale
	wx, wy := e.windowX, e.windowY
	if e.resizeEdge == "" {
		wx += dx
		wy += dy
	} else {
		width, height := e.windowWidth, e.windowHeight
		if strings.Contains(e.resizeEdge, "e") {
			width += dx
		}
		if strings.Contains(e.resizeEdge, "w") {
			width -= dx
		}
		if strings.Contains(e.resizeEdge, "s") {
			height += dy
		}
		if strings.Contains(e.resizeEdge, "n") {
			height -= dy
		}
		opts := e.parent.parent.options
		width = max(width, max(1, opts.MinWidth))
		height = max(height, max(1, opts.MinHeight))
		if opts.MaxWidth > 0 {
			width = min(width, opts.MaxWidth)
		}
		if opts.MaxHeight > 0 {
			height = min(height, opts.MaxHeight)
		}
		if strings.Contains(e.resizeEdge, "w") {
			wx += e.windowWidth - width
		}
		if strings.Contains(e.resizeEdge, "n") {
			wy += e.windowHeight - height
		}
		C.gtk_window_resize((*C.GtkWindow)(e.parent.window), C.int(width), C.int(height))
	}
	C.gtk_window_move((*C.GtkWindow)(e.parent.window), C.int(wx), C.int(wy))
}
