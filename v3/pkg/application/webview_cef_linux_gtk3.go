//go:build linux && cgo && gtk3 && wails_cef && !android && !server

package application

/*
#cgo linux pkg-config: gtk+-3.0 webkit2gtk-4.1 gdk-3.0 x11
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

// wails_cef_create_host_child creates a RAW X child window under the
// drawing area's X window. CEF's windowed browser cannot parent into a
// GDK-owned window (its presenter ends up targeting the wrong window and
// fails with XGetWindowAttributes errors); a plain X child works (same
// finding as the pure-C host experiments).
// wails_cef_create_host_child creates the raw X window that parents the
// browser. It must be a CHILD OF ROOT positioned over the drawing area:
// with a GDK window anywhere in its ancestor chain CEF's presenter
// fails (verified experimentally; root-direct children work).
static unsigned long wails_cef_create_host_child(GtkWidget* widget, int w, int h) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL) return 0;
  Display* d = gdk_x11_get_default_xdisplay();
  gint ox = 0, oy = 0;
  gdk_window_get_origin(win, &ox, &oy);
  Window root = DefaultRootWindow(d);
  Window child = XCreateSimpleWindow(d, root, ox, oy, (unsigned)w, (unsigned)h, 0, 0, 0);
  XMapWindow(d, child);
  // Keep the host directly above the GTK toplevel in the stacking order.
  GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  if (top != NULL) {
    Window order[2] = { (Window)gdk_x11_window_get_xid(top), child };
    XRestackWindows(d, order, 2);
  }
  XFlush(d);
  return (unsigned long)child;
}

// wails_cef_sync_host moves/resizes the host window to track the drawing
// area (origin change from window moves, size from size-allocate).
static void wails_cef_sync_host(GtkWidget* widget, unsigned long host) {
  GdkWindow* win = gtk_widget_get_window(widget);
  if (win == NULL || host == 0) return;
  Display* d = gdk_x11_get_default_xdisplay();
  gint ox = 0, oy = 0;
  gdk_window_get_origin(win, &ox, &oy);
  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);
  XMoveResizeWindow(d, (Window)host, ox, oy, (unsigned)a.width, (unsigned)a.height);
  GdkWindow* top = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  if (top != NULL) {
    Window order[2] = { (Window)gdk_x11_window_get_xid(top), (Window)host };
    XRestackWindows(d, order, 2);
  }
  XFlush(d);
}

// wails_cef_destroy_window unmaps and destroys an X window.
static void wails_cef_destroy_window(unsigned long xwin) {
  if (xwin == 0) return;
  Display* d = gdk_x11_get_default_xdisplay();
  XDestroyWindow(d, (Window)xwin);
  XFlush(d);
}

// wails_cef_resize_window resizes an X window (the host child or the
// browser window found under it).
static void wails_cef_resize_window(unsigned long xwin, int w, int h) {
  if (xwin == 0) return;
  Display* d = gdk_x11_get_default_xdisplay();
  XResizeWindow(d, (Window)xwin, (unsigned)w, (unsigned)h);
  XFlush(d);
}

// wails_cef_child_of returns the first child window of the given X
// window (0 if none) — used to find the browser window for resizing.
static unsigned long wails_cef_child_of(unsigned long xwin) {
  if (xwin == 0) return 0;
  Display* d = gdk_x11_get_default_xdisplay();
  Window r, p, *kids = NULL; unsigned int n = 0;
  unsigned long out = 0;
  if (XQueryTree(d, (Window)xwin, &r, &p, &kids, &n) && n > 0) out = kids[0];
  if (kids) XFree(kids);
  return out;
}

// wails_cef_toplevel_xid returns the X11 window of the widget's
// toplevel window.
static unsigned long wails_cef_toplevel_xid(GtkWidget* widget) {
  GtkWidget* top = gtk_widget_get_toplevel(widget);
  GdkWindow* win = gtk_widget_get_window(top);
  if (win == NULL) return 0;
  return (unsigned long)gdk_x11_window_get_xid(win);
}

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
	"os"
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

	// hostChild is the raw X child window under the drawing area that
	// parents the CEF browser (GDK-owned parents break CEF's presenter).
	hostChild uintptr

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
func newCEFWebview(windowId uint, _ WebviewGpuPolicy) pointer {
	widget := pointer(C.gtk_drawing_area_new())
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
	done := e.attached || e.creating || e.startURL == "" || e.browser != nil
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
	if e.attached || e.creating {
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

	fmt.Fprintf(os.Stderr, "[cef-attach] attempt: startURL=%q\n", e.startURL)
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
	// CEF cannot parent into a GDK-owned window; host it on a raw X child.
	winWidth0, winHeight0 := e.parent.size()
	hostChild := uintptr(C.wails_cef_create_host_child((*C.GtkWidget)(e.widget), C.int(winWidth0), C.int(winHeight0)))
	if hostChild == 0 {
		e.attachErr = fmt.Errorf("CEF: cannot create host child window")
		return
	}
	e.mu.Lock()
	e.hostChild = hostChild
	e.mu.Unlock()
	xid = hostChild
	// Forward toplevel keyboard focus to the browser window once it exists
	// (see focusBrowser).
	if e.parent != nil {
		C.wails_cef_connect_focus((*C.GtkWidget)(e.parent.window))
	}

	winWidth, winHeight := e.parent.size()
	fmt.Fprintf(os.Stderr, "[cef-attach] creating browser host=0x%x url=%s\n", hostChild, e.startURL)
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
	browser := e.browser
	e.browser = nil
	host := e.hostChild
	e.hostChild = 0
	e.mu.Unlock()
	if browser != nil {
		browser.Close(false)
	}
	// The host window is a root child; remove it so it cannot outlive the
	// GTK window it was overlaying.
	if host != 0 {
		C.wails_cef_destroy_window(C.ulong(host))
	}
}

// resizeBrowser sizes the native browser window to match the container.
// Called from the GTK main thread (size-allocate) and right after attach.
func (e *linuxCEFWebview) resizeBrowser(width, height int) {
	if width <= 1 || height <= 1 {
		return
	}
	e.mu.Lock()
	lastW, lastH := e.lastWidth, e.lastHeight
	e.lastWidth, e.lastHeight = width, height
	e.mu.Unlock()
	if (lastW == width && lastH == height) || e.hostChild == 0 {
		return
	}
	C.wails_cef_sync_host((*C.GtkWidget)(e.widget), C.ulong(e.hostChild))
	if child := uintptr(C.wails_cef_child_of(C.ulong(e.hostChild))); child != 0 {
		C.wails_cef_resize_window(C.ulong(child), C.int(width), C.int(height))
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
