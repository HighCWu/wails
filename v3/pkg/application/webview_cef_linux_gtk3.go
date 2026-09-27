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

// wails_cef_query_pointer returns the current pointer position relative
// to the root window plus the button state mask, used to synthesize drag
// state for gtk_window_begin_move_drag when GTK never saw the button
// press (it happened inside CEF's native child window).
static int wails_cef_query_pointer(GtkWidget* widget, int* btn, int* root_x, int* root_y) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL) return 0;
  Display* d = gdk_x11_get_default_xdisplay();
  Window root = XDefaultRootWindow(d);
  Window rret, cret;
  int wx, wy;
  unsigned int mask = 0;
  if (!XQueryPointer(d, root, &rret, &cret, root_x, root_y, &wx, &wy, &mask)) return 0;
  int button = 0;
  if (mask & Button1Mask) button = 1;
  else if (mask & Button2Mask) button = 2;
  else if (mask & Button3Mask) button = 3;
  *btn = button;
  return 1;
}

// wails_cef_focus_xwin forwards the X keyboard focus to the embedded
// browser's native child window: the WM focuses the GTK toplevel, and X
// delivers key events to the focused window — CEF's window would never
// see them otherwise.
static unsigned long wails_cef_focus_input_child(GtkWidget* widget) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL) return 0;
  Display* d = gdk_x11_get_default_xdisplay();
  Window w = (Window)gdk_x11_window_get_xid(win);
  Window r, p, *kids = NULL; unsigned int n = 0;
  if (!XQueryTree(d, w, &r, &p, &kids, &n) || n == 0) return 0;
  unsigned long target = 0;
  for (unsigned int i = 0; i < n; i++) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(d, kids[i], &attr)) continue;
    if (attr.width > 2) { target = kids[i]; break; }
    if (target == 0) target = kids[i];
  }
  XFree(kids);
  if (target != 0) XSetInputFocus(d, (Window)target, RevertToParent, CurrentTime);
  return target;
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

// wails_cef_connect_focus forwards toplevel focus-in to the Go callback,
// which moves the X focus onto the browser window.
extern gboolean wailsCEFOnFocusIn(GtkWidget* widget, GdkEvent* event, gpointer user_data);
static void wails_cef_connect_focus(GtkWidget* widget) {
  g_signal_connect(widget, "focus-in-event", G_CALLBACK(wailsCEFOnFocusIn), NULL);
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

	// background (0xAARRGGBB, 0 = CEF default) applied at browser
	// creation; CEF latches it with the first paint.
	background uint32

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
	// Forward toplevel keyboard focus to the browser window once it exists
	// (see focusBrowser).
	if e.parent != nil {
		C.wails_cef_connect_focus((*C.GtkWidget)(e.parent.window))
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
	C.wails_cef_focus_input_child((*C.GtkWidget)(e.widget))
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
