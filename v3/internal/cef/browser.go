//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}

// Shims over the //export'ed callbacks in handlers.go.

// cef_client_t
struct _cef_life_span_handler_t* wailsCEFClientGetLSH(struct _cef_client_t* self);
static struct _cef_life_span_handler_t* wails_cef_client_get_lsh(struct _cef_client_t* self) {
  return wailsCEFClientGetLSH(self);
}
struct _cef_load_handler_t* wailsCEFClientGetLoadH(struct _cef_client_t* self);
static struct _cef_load_handler_t* wails_cef_client_get_loadh(struct _cef_client_t* self) {
  return wailsCEFClientGetLoadH(self);
}
struct _cef_display_handler_t* wailsCEFClientGetDisplayH(struct _cef_client_t* self);
static struct _cef_display_handler_t* wails_cef_client_get_displayh(struct _cef_client_t* self) {
  return wailsCEFClientGetDisplayH(self);
}
int wailsCEFClientProcessMessage(struct _cef_client_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, cef_process_id_t source_process, struct _cef_process_message_t* message);
static int wails_cef_client_process_message(struct _cef_client_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, cef_process_id_t source_process, struct _cef_process_message_t* message) {
  return wailsCEFClientProcessMessage(self, browser, frame, source_process, message);
}

// cef_life_span_handler_t
void wailsCEFLSHAfterCreated(struct _cef_life_span_handler_t* self, struct _cef_browser_t* browser);
static void wails_cef_lsh_after_created(struct _cef_life_span_handler_t* self, struct _cef_browser_t* browser) {
  wailsCEFLSHAfterCreated(self, browser);
}
int wailsCEFLSHDoClose(struct _cef_life_span_handler_t* self, struct _cef_browser_t* browser);
static int wails_cef_lsh_do_close(struct _cef_life_span_handler_t* self, struct _cef_browser_t* browser) {
  return wailsCEFLSHDoClose(self, browser);
}
void wailsCEFLSHBeforeClose(struct _cef_life_span_handler_t* self, struct _cef_browser_t* browser);
static void wails_cef_lsh_before_close(struct _cef_life_span_handler_t* self, struct _cef_browser_t* browser) {
  wailsCEFLSHBeforeClose(self, browser);
}

// cef_load_handler_t
void wailsCEFLoadHOnLoadEnd(struct _cef_load_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, int http_code);
static void wails_cef_loadh_on_load_end(struct _cef_load_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, int http_code) {
  wailsCEFLoadHOnLoadEnd(self, browser, frame, http_code);
}
void wailsCEFLoadHOnLoadError(struct _cef_load_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, int error_code, const cef_string_t* error_text, const cef_string_t* failed_url);
static void wails_cef_loadh_on_load_error(struct _cef_load_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, int error_code, const cef_string_t* error_text, const cef_string_t* failed_url) {
  wailsCEFLoadHOnLoadError(self, browser, frame, error_code, error_text, failed_url);
}

// cef_display_handler_t
void wailsCEFDisplayHOnTitleChange(struct _cef_display_handler_t* self, struct _cef_browser_t* browser, const cef_string_t* title);
static void wails_cef_displayh_on_title_change(struct _cef_display_handler_t* self, struct _cef_browser_t* browser, const cef_string_t* title) {
  wailsCEFDisplayHOnTitleChange(self, browser, title);
}

// Struct initialisers (see app.go for why wiring happens in C).
static void wcef_init_client(void* p) {
  cef_client_t* c = (cef_client_t*)p;
  c->get_life_span_handler = wails_cef_client_get_lsh;
  c->get_load_handler = wails_cef_client_get_loadh;
  c->get_display_handler = wails_cef_client_get_displayh;
  c->on_process_message_received = wails_cef_client_process_message;
}
static void wcef_init_lsh(void* p) {
  cef_life_span_handler_t* h = (cef_life_span_handler_t*)p;
  h->on_after_created = wails_cef_lsh_after_created;
  h->do_close = wails_cef_lsh_do_close;
  h->on_before_close = wails_cef_lsh_before_close;
}
static void wcef_init_loadh(void* p) {
  cef_load_handler_t* h = (cef_load_handler_t*)p;
  h->on_load_end = wails_cef_loadh_on_load_end;
  h->on_load_error = wails_cef_loadh_on_load_error;
}
static void wcef_init_displayh(void* p) {
  cef_display_handler_t* h = (cef_display_handler_t*)p;
  h->on_title_change = wails_cef_displayh_on_title_change;
}
*/
import "C"

import (
	"log/slog"
	"os"
	"sync"
	"unsafe"
)

// pkgLogger gives CEF-originated messages a consistent destination.
func pkgLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelWarn}))
}

// Browser tracks one CEF browser instance created for a wails window.
type Browser struct {
	c        *C.cef_browser_t // addref'd in on_after_created
	host     *C.cef_browser_host_t
	windowID uint
	client   *browserClient
}

var (
	browsersByID  sync.Map // int(cef browser identifier) -> *Browser
	browsersByWin sync.Map // uint(wails window id) -> *Browser
)

func lookupBrowserByCefID(id int) *Browser {
	if v, ok := browsersByID.Load(id); ok {
		return v.(*Browser)
	}
	return nil
}

// LookupBrowser returns the browser attached to a wails window, if any.
func LookupBrowser(windowID uint) *Browser {
	if v, ok := browsersByWin.Load(windowID); ok {
		return v.(*Browser)
	}
	return nil
}

// browserClient owns the per-window CEF handler structs.
type browserClient struct {
	c        *C.cef_client_t
	lsh      *C.cef_life_span_handler_t
	loadH    *C.cef_load_handler_t
	displayH *C.cef_display_handler_t
	windowID uint
}

func newBrowserClient(windowID uint) *browserClient {
	bc := &browserClient{windowID: windowID}

	bc.lsh = (*C.cef_life_span_handler_t)(allocStruct(C.sizeof_cef_life_span_handler_t))
	C.wcef_init_lsh(unsafe.Pointer(bc.lsh))

	bc.loadH = (*C.cef_load_handler_t)(allocStruct(C.sizeof_cef_load_handler_t))
	C.wcef_init_loadh(unsafe.Pointer(bc.loadH))

	bc.displayH = (*C.cef_display_handler_t)(allocStruct(C.sizeof_cef_display_handler_t))
	C.wcef_init_displayh(unsafe.Pointer(bc.displayH))

	bc.c = (*C.cef_client_t)(allocStruct(C.sizeof_cef_client_t))
	C.wcef_init_client(unsafe.Pointer(bc.c))

	handlerOwners.Store(unsafe.Pointer(bc.lsh), bc)
	handlerOwners.Store(unsafe.Pointer(bc.loadH), bc)
	handlerOwners.Store(unsafe.Pointer(bc.displayH), bc)
	return bc
}

func clientByPtr(p unsafe.Pointer) *browserClient {
	if v, ok := clients.Load(p); ok {
		return v.(*browserClient)
	}
	return nil
}

// clientByHandlerPtr resolves the owning client from any of its handler
// struct pointers (life span / load / display).
func clientByHandlerPtr(p unsafe.Pointer) *browserClient {
	if v, ok := handlerOwners.Load(p); ok {
		return v.(*browserClient)
	}
	return nil
}

var (
	clients       sync.Map // unsafe.Pointer(cef_client_t) -> *browserClient
	handlerOwners sync.Map // unsafe.Pointer(any handler struct) -> *browserClient
)

// CreateBrowserOptions describes an embedded (windowed) browser.
type CreateBrowserOptions struct {
	// WindowID is the wails window the browser is embedded into.
	WindowID uint
	// ParentXWindow is the X11 window (XID) to embed into; on Linux CEF
	// parents its native browser window onto it.
	ParentXWindow uintptr
	// URL is the initial page.
	URL string
	// Width/Height are the initial bounds within the parent.
	Width, Height int
}

// CreateBrowser creates a windowed browser embedded into the given X11
// window. Returns once the create call is dispatched; on_after_created
// completes the wiring asynchronously.
func CreateBrowser(opts CreateBrowserOptions) (*Browser, error) {
	if !Initialized() {
		return nil, errNotInitialized
	}

	bc := newBrowserClient(opts.WindowID)
	clients.Store(unsafe.Pointer(bc.c), bc)

	wi := (*C.cef_window_info_t)(C.calloc(1, C.sizeof_cef_window_info_t))
	wi.size = C.sizeof_cef_window_info_t
	wi.bounds.x = C.int(0)
	wi.bounds.y = C.int(0)
	wi.bounds.width = C.int(opts.Width)
	wi.bounds.height = C.int(opts.Height)
	wi.parent_window = C.cef_window_handle_t(opts.ParentXWindow)
	wi.windowless_rendering_enabled = 0
	defer C.free(unsafe.Pointer(wi))

	settings := (*C.cef_browser_settings_t)(C.calloc(1, C.sizeof_cef_browser_settings_t))
	settings.size = C.sizeof_cef_browser_settings_t
	defer C.free(unsafe.Pointer(settings))

	url := newCefString(opts.URL)
	defer url.Clear()

	// The pending browser is completed by on_after_created (see
	// lifeSpanAfterCreated); associate the client with the window now so
	// the mapping exists the moment the first scheme request arrives.
	b := &Browser{windowID: opts.WindowID, client: bc}
	pendingBrowsers.Store(opts.WindowID, b)

	if C.wcef_create_browser(wi, bc.c, url.ptr(), settings) != 1 {
		pendingBrowsers.Delete(opts.WindowID)
		return nil, errCreateBrowser
	}
	return b, nil
}

var (
	pendingBrowsers sync.Map // uint windowID -> *Browser (until on_after_created)
)

var (
	errNotInitialized = errString("cef: not initialized")
	errCreateBrowser  = errString("cef: cef_browser_host_create_browser failed")
)

type errString string

func (e errString) Error() string { return string(e) }

// lifeSpanAfterCreated implements
// cef_life_span_handler_t.on_after_created.
func lifeSpanAfterCreated(browser *C.cef_browser_t, client *browserClient) {
	C.wcef_obj_add_ref(unsafe.Pointer(browser))
	b := &Browser{
		c:        browser,
		host:     C.wcef_browser_get_host(browser),
		windowID: client.windowID,
		client:   client,
	}
	browsersByID.Store(int(C.wcef_browser_get_identifier(browser)), b)
	if pending, ok := pendingBrowsers.LoadAndDelete(client.windowID); ok {
		// Transfer any pre-created state (none today) — the canonical
		// mapping is stored below either way.
		_ = pending
	}
	browsersByWin.Store(client.windowID, b)
}

// lifeSpanDoClose implements cef_life_span_handler_t.do_close: let CEF
// close the browser (and its native child window) normally.
func lifeSpanDoClose(browser *C.cef_browser_t, client *browserClient) C.int {
	_ = browser
	_ = client
	return 0
}

// lifeSpanBeforeClose implements cef_life_span_handler_t.on_before_close.
func lifeSpanBeforeClose(browser *C.cef_browser_t, client *browserClient) {
	id := int(C.wcef_browser_get_identifier(browser))
	if b, ok := browsersByID.LoadAndDelete(id); ok {
		browser := b.(*Browser)
		browsersByWin.Delete(browser.windowID)
		C.wcef_obj_release(unsafe.Pointer(browser.c))
	}
	st := state.Load()
	if st != nil && st.OnBrowserClosed != nil {
		st.OnBrowserClosed(client.windowID)
	}
}

// loadEnd implements cef_load_handler_t.on_load_end for the main frame:
// maps to the wails WindowLoadFinished event.
func loadEnd(browser *C.cef_browser_t, frame *C.cef_frame_t, httpCode int, client *browserClient) {
	_ = httpCode
	if frame == nil || C.wcef_frame_is_main(frame) != 1 {
		return
	}
	st := state.Load()
	if st != nil && st.OnWindowLoadEnd != nil {
		st.OnWindowLoadEnd(client.windowID)
	}
}

// loadError implements cef_load_handler_t.on_load_error (main frame only;
// subframe errors are ignored).
func loadError(browser *C.cef_browser_t, frame *C.cef_frame_t, errorCode int, errorText *C.cef_string_t, failedURL *C.cef_string_t, client *browserClient) {
	_ = browser
	_ = client
	if frame != nil && C.wcef_frame_is_main(frame) != 1 {
		return
	}
	if logger := pkgLogger(); logger != nil {
		logger.Error("CEF load error", "code", errorCode, "text", goString(errorText), "url", goString(failedURL))
	}
}

// titleChange implements cef_display_handler_t.on_title_change.
func titleChange(browser *C.cef_browser_t, title *C.cef_string_t, client *browserClient) {
	st := state.Load()
	if st != nil && st.OnTitleChange != nil {
		st.OnTitleChange(client.windowID, goString(title))
	}
}

// ExecJS evaluates JavaScript in the browser's main frame. CEF marshals
// execution onto its UI thread internally.
func (b *Browser) ExecJS(js string) {
	if b == nil || b.c == nil || C.wcef_browser_is_valid(b.c) != 1 {
		return
	}
	frame := C.wcef_browser_get_main_frame(b.c)
	if frame == nil || C.wcef_frame_is_valid(frame) != 1 {
		return
	}
	code := newCefString(js)
	defer code.Clear()
	url := newCefString("wails://localhost/wails/exec")
	defer url.Clear()
	C.wcef_frame_exec_js(frame, code.ptr(), url.ptr(), 1)
}

// Close destroys the browser. The native child window is removed
// asynchronously; OnBrowserClosed fires when it completes.
func (b *Browser) Close(force bool) {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_close_browser(b.host, gtkBoolC(force))
}

// XWindow returns the browser's native X11 window handle (0 while the
// browser has not created it yet).
func (b *Browser) XWindow() uintptr {
	if b == nil || b.host == nil {
		return 0
	}
	return uintptr(C.wcef_host_get_window_handle(b.host))
}

func gtkBoolC(v bool) C.int {
	if v {
		return 1
	}
	return 0
}
