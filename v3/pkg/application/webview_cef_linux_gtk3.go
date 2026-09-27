//go:build linux && cgo && gtk3 && wails_cef && !android && !server

package application

/*
#cgo linux pkg-config: gtk+-3.0 webkit2gtk-4.1 gdk-3.0 x11
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

// wails_cef_widget_xid returns the X11 window of a realised widget.
static unsigned long wails_cef_widget_xid(GtkWidget* widget) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL) return 0;
  return (unsigned long)gdk_x11_window_get_xid(win);
}

// wails_cef_resize_browser resizes the browser's native child window to
// match the GTK container allocation. CEF windowed browsers on X11 do not
// track their parent automatically.
static void wails_cef_resize_browser(unsigned long xwin, int w, int h) {
  if (xwin == 0) return;
  Display* d = gdk_x11_get_default_xdisplay();
  XResizeWindow(d, (Window)xwin, (unsigned)w, (unsigned)h);
}

// wails_cef_widget_size returns the widget's current allocation.
static void wails_cef_widget_size(GtkWidget* widget, int* w, int* h) {
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  *w = a.width;
  *h = a.height;
}

// wails_cef_connect_map runs fn (an //export'ed Go callback) when the
// widget is first mapped, i.e. its GdkWindow exists.
extern void wailsCEFOnMap(GtkWidget* widget, gpointer user_data);
static void wails_cef_connect_map(GtkWidget* widget, gpointer user_data) {
  g_signal_connect(widget, "map", G_CALLBACK(wailsCEFOnMap), user_data);
}

// wails_cef_connect_size_allocate tracks allocation changes so the
// embedded browser window can be resized to match.
extern void wailsCEFOnSizeAllocate(GtkWidget* widget, GdkRectangle* allocation, gpointer user_data);
static void wails_cef_connect_size_allocate(GtkWidget* widget, gpointer user_data) {
  g_signal_connect(widget, "size-allocate", G_CALLBACK(wailsCEFOnSizeAllocate), user_data);
}
*/
import "C"

import (
	"fmt"
	"sync"
	"unsafe"

	"github.com/wailsapp/wails/v3/internal/cef"
)

// linuxCEFWebview is the CEF flavour of the in-window webview engine: a
// plain GTK drawing area whose X11 window hosts CEF's native browser
// window. All webview-facing linuxWebviewWindow methods dispatch here
// when w.cefEngine != nil.
type linuxCEFWebview struct {
	parent   *linuxWebviewWindow
	widget   pointer // GtkDrawingArea (owns the X11 host window)
	windowID uint

	mu            sync.Mutex
	browser       *cef.Browser
	startURL      string
	attached      bool
	attachErr     error
	lastWidth     int
	lastHeight    int
}

// cefEngines maps the GTK widget pointer to its engine so the C callback
// trampolines can find the Go side.
var cefEngines sync.Map // unsafe.Pointer(widget) -> *linuxCEFWebview

func init() {
	createWindowWebview = newCEFWebview
}

// newCEFWebview creates the GTK container widget used in place of a
// WebKitWebView. The CEF browser itself is created when the widget is
// mapped (its X11 window only exists then).
func newCEFWebview(windowId uint, _ WebviewGpuPolicy) pointer {
	widget := pointer(C.gtk_drawing_area_new())
	C.gtk_widget_set_can_focus((*C.GtkWidget)(widget), C.gboolean(1))
	e := &linuxCEFWebview{widget: widget, windowID: windowId}
	cefEngines.Store(unsafe.Pointer(widget), e)
	C.wails_cef_connect_map((*C.GtkWidget)(widget), nil)
	C.wails_cef_connect_size_allocate((*C.GtkWidget)(widget), nil)
	return widget
}

// attach creates the CEF browser inside the widget's X11 window. Called
// on the GTK main thread from the map handler.
func (e *linuxCEFWebview) attach() {
	e.mu.Lock()
	if e.attached {
		e.mu.Unlock()
		return
	}
	e.attached = true
	e.mu.Unlock()

	xid := uintptr(C.wails_cef_widget_xid((*C.GtkWidget)(e.widget)))
	if xid == 0 {
		e.attachErr = fmt.Errorf("CEF: widget has no X11 window")
		return
	}

	winWidth, winHeight := e.parent.size()
	browser, err := cef.CreateBrowser(cef.CreateBrowserOptions{
		WindowID:       e.windowID,
		ParentXWindow:  xid,
		URL:            e.startURL,
		Width:          winWidth,
		Height:         winHeight,
	})
	if err != nil {
		e.attachErr = err
		globalApplication.handleFatalError(fmt.Errorf("CEF backend: %w", err))
		return
	}
	e.mu.Lock()
	e.browser = browser
	e.mu.Unlock()

	// The GTK window is already visible at this point; size the browser to
	// the current allocation in case it arrived before the browser did.
	var cw, ch C.int
	C.wails_cef_widget_size((*C.GtkWidget)(e.widget), &cw, &ch)
	if cw > 1 && ch > 1 {
		e.resizeBrowser(int(cw), int(ch))
	}
}

// setStartURL records the URL to load when the browser is created (or
// navigates immediately if it already exists).
func (e *linuxCEFWebview) setStartURL(url string) {
	e.startURL = url
	e.mu.Lock()
	browser := e.browser
	e.mu.Unlock()
	if browser != nil {
		e.execJS(fmt.Sprintf("window.location.href = %q;", url))
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
	browser := e.browser
	e.browser = nil
	e.mu.Unlock()
	if browser != nil {
		browser.Close(false)
	}
}

// resizeBrowser sizes the native browser window to match the container.
// Called from the GTK main thread (size-allocate) and right after attach.
func (e *linuxCEFWebview) resizeBrowser(width, height int) {
	if width <= 1 || height <= 1 {
		return
	}
	e.mu.Lock()
	browser := e.browser
	lastW, lastH := e.lastWidth, e.lastHeight
	e.lastWidth, e.lastHeight = width, height
	e.mu.Unlock()
	if browser == nil || (lastW == width && lastH == height) {
		return
	}
	if xwin := browser.XWindow(); xwin != 0 {
		C.wails_cef_resize_browser(C.ulong(xwin), C.int(width), C.int(height))
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

// attachCEFEngine links the engine created by windowNewCEFWebview to its
// window. Called from run() right after windowNew.
func (w *linuxWebviewWindow) attachCEFEngine() {
	if e, ok := cefEngines.Load(unsafe.Pointer(w.webview)); ok {
		engine := e.(*linuxCEFWebview)
		engine.parent = w
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
