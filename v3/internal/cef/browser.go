//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15400

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

// cef_keyboard_handler_t
int wailsCEFKeyboardOnKeyEvent(struct _cef_keyboard_handler_t* self, struct _cef_browser_t* browser, const cef_key_event_t* event, cef_event_handle_t os_event);
static int wails_cef_kb_on_key_event(struct _cef_keyboard_handler_t* self, struct _cef_browser_t* browser, const cef_key_event_t* event, cef_event_handle_t os_event) {
 return wailsCEFKeyboardOnKeyEvent(self,browser,event,os_event);
}
static int wails_cef_kb_on_pre_key_event(struct _cef_keyboard_handler_t* self, struct _cef_browser_t* browser, const cef_key_event_t* event, cef_event_handle_t os_event, int* is_keyboard_shortcut) {
  return wailsCEFKeyboardOnKeyEvent(self, browser, event, os_event);
}

// cef_permission_handler_t
int wailsCEFPermissionOnMediaAccess(struct _cef_permission_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, const cef_string_t* requesting_origin, uint32_t requested_permissions, struct _cef_media_access_callback_t* callback);
static int wails_cef_perm_on_media_access(struct _cef_permission_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, const cef_string_t* requesting_origin, uint32_t requested_permissions, struct _cef_media_access_callback_t* callback) {
  return wailsCEFPermissionOnMediaAccess(self, browser, frame, requesting_origin, requested_permissions, callback);
}

// cef_drag_handler_t
int wailsCEFDragOnEnter(struct _cef_drag_handler_t* self, struct _cef_browser_t* browser, struct _cef_drag_data_t* drag_data, int mask);
static int wails_cef_drag_on_enter(struct _cef_drag_handler_t* self, struct _cef_browser_t* browser, struct _cef_drag_data_t* drag_data, cef_drag_operations_mask_t mask) {
  return wailsCEFDragOnEnter(self, browser, drag_data, (int)mask);
}

// cef_request_handler_t
int wailsCEFRequestOnBeforeBrowse(struct _cef_request_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, struct _cef_request_t* request, int user_gesture, int is_redirect);
static int wails_cef_req_on_before_browse(struct _cef_request_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, struct _cef_request_t* request, int user_gesture, int is_redirect) {
  return wailsCEFRequestOnBeforeBrowse(self, browser, frame, request, user_gesture, is_redirect);
}

void wailsCEFRenderTerminated(struct _cef_request_handler_t* self, struct _cef_browser_t* browser, int status, int error_code, const cef_string_t* error_string);
static void wails_cef_render_terminated(struct _cef_request_handler_t* self, struct _cef_browser_t* browser, cef_termination_status_t status, int error_code, const cef_string_t* error_string) {
 wailsCEFRenderTerminated(self, browser, (int)status, error_code, error_string);
}

// client getters for the new handlers
struct _cef_keyboard_handler_t* wailsCEFClientGetKeyboardH(struct _cef_client_t* self);
static struct _cef_keyboard_handler_t* wails_cef_client_get_keyboardh(struct _cef_client_t* self) {
  return wailsCEFClientGetKeyboardH(self);
}
struct _cef_permission_handler_t* wailsCEFClientGetPermissionH(struct _cef_client_t* self);
static struct _cef_permission_handler_t* wails_cef_client_get_permissionh(struct _cef_client_t* self) {
  return wailsCEFClientGetPermissionH(self);
}
struct _cef_drag_handler_t* wailsCEFClientGetDragH(struct _cef_client_t* self);
static struct _cef_drag_handler_t* wails_cef_client_get_dragh(struct _cef_client_t* self) {
  return wailsCEFClientGetDragH(self);
}
struct _cef_request_handler_t* wailsCEFClientGetRequestH(struct _cef_client_t* self);
static struct _cef_request_handler_t* wails_cef_client_get_requesth(struct _cef_client_t* self) {
  return wailsCEFClientGetRequestH(self);
}

// Struct initialisers (see app.go for why wiring happens in C).
static void wcef_init_client(void* p) {
  cef_client_t* c = (cef_client_t*)p;
  c->get_life_span_handler = wails_cef_client_get_lsh;
  c->get_load_handler = wails_cef_client_get_loadh;
  c->get_display_handler = wails_cef_client_get_displayh;
  c->get_keyboard_handler = wails_cef_client_get_keyboardh;
  c->get_permission_handler = wails_cef_client_get_permissionh;
  c->get_drag_handler = wails_cef_client_get_dragh;
  c->get_request_handler = wails_cef_client_get_requesth;
  c->on_process_message_received = wails_cef_client_process_message;
}
static void wcef_init_keyboardh(void* p) {
  cef_keyboard_handler_t* h = (cef_keyboard_handler_t*)p;
  #if defined(OS_LINUX)
  h->on_key_event = wails_cef_kb_on_key_event;
#else
  h->on_pre_key_event = wails_cef_kb_on_pre_key_event;
#endif
}
static void wcef_init_permissionh(void* p) {
  cef_permission_handler_t* h = (cef_permission_handler_t*)p;
  h->on_request_media_access_permission = wails_cef_perm_on_media_access;
}
static void wcef_init_dragh(void* p) {
  cef_drag_handler_t* h = (cef_drag_handler_t*)p;
  h->on_drag_enter = wails_cef_drag_on_enter;
}
static void wcef_init_requesth(void* p) {
  cef_request_handler_t* h = (cef_request_handler_t*)p;
  h->on_before_browse = wails_cef_req_on_before_browse;
 h->on_render_process_terminated = wails_cef_render_terminated;
}
static void wcef_init_lsh(void* p) {
  cef_life_span_handler_t* h = (cef_life_span_handler_t*)p;
  h->on_after_created = wails_cef_lsh_after_created;
  h->do_close = wails_cef_lsh_do_close;
  h->on_before_close = wails_cef_lsh_before_close;
}
void wailsCEFLoadHOnLoadStart(struct _cef_load_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, int transition_type);
static void wails_cef_loadh_on_load_start(struct _cef_load_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, cef_transition_type_t transition_type) {
  wailsCEFLoadHOnLoadStart(self, browser, frame, (int)transition_type);
}
static void wcef_init_loadh(void* p) {
  cef_load_handler_t* h = (cef_load_handler_t*)p;
  h->on_load_start = wails_cef_loadh_on_load_start;
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
	"math"
	"os"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// pkgLogger gives CEF-originated messages a consistent destination.
func pkgLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelWarn}))
}

// Browser tracks one CEF browser instance created for a wails window.
type Browser struct {
	c              *C.cef_browser_t // addref'd in on_after_created
	host           *C.cef_browser_host_t
	windowID       uint
	client         *browserClient
	devToolsClient *browserClient
	closeRequested bool

	// lastCrash gates the single automatic reload after a renderer crash.
	lastCrash time.Time
	crashMu   sync.Mutex
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
	c           *C.cef_client_t
	lsh         *C.cef_life_span_handler_t
	loadH       *C.cef_load_handler_t
	displayH    *C.cef_display_handler_t
	keyboardH   *C.cef_keyboard_handler_t
	permissionH *C.cef_permission_handler_t
	dragH       *C.cef_drag_handler_t
	requestH    *C.cef_request_handler_t
	windowID    uint

	// dragPending remembers native file drags so any fallback file://
	// navigation can be blocked. GTK delivers actual drops with coordinates.
	dragPending []string
	dragMu      sync.Mutex
}

func newBrowserClient(windowID uint) *browserClient {
	bc := &browserClient{windowID: windowID}

	bc.lsh = (*C.cef_life_span_handler_t)(allocStruct(C.sizeof_cef_life_span_handler_t))
	C.wcef_init_lsh(unsafe.Pointer(bc.lsh))

	bc.loadH = (*C.cef_load_handler_t)(allocStruct(C.sizeof_cef_load_handler_t))
	C.wcef_init_loadh(unsafe.Pointer(bc.loadH))

	bc.displayH = (*C.cef_display_handler_t)(allocStruct(C.sizeof_cef_display_handler_t))
	C.wcef_init_displayh(unsafe.Pointer(bc.displayH))

	bc.keyboardH = (*C.cef_keyboard_handler_t)(allocStruct(C.sizeof_cef_keyboard_handler_t))
	C.wcef_init_keyboardh(unsafe.Pointer(bc.keyboardH))

	bc.permissionH = (*C.cef_permission_handler_t)(allocStruct(C.sizeof_cef_permission_handler_t))
	C.wcef_init_permissionh(unsafe.Pointer(bc.permissionH))

	bc.dragH = (*C.cef_drag_handler_t)(allocStruct(C.sizeof_cef_drag_handler_t))
	C.wcef_init_dragh(unsafe.Pointer(bc.dragH))

	bc.requestH = (*C.cef_request_handler_t)(allocStruct(C.sizeof_cef_request_handler_t))
	C.wcef_init_requesth(unsafe.Pointer(bc.requestH))

	bc.c = (*C.cef_client_t)(allocStruct(C.sizeof_cef_client_t))
	C.wcef_init_client(unsafe.Pointer(bc.c))

	handlerOwners.Store(unsafe.Pointer(bc.lsh), bc)
	handlerOwners.Store(unsafe.Pointer(bc.loadH), bc)
	handlerOwners.Store(unsafe.Pointer(bc.displayH), bc)
	handlerOwners.Store(unsafe.Pointer(bc.keyboardH), bc)
	handlerOwners.Store(unsafe.Pointer(bc.permissionH), bc)
	handlerOwners.Store(unsafe.Pointer(bc.dragH), bc)
	handlerOwners.Store(unsafe.Pointer(bc.requestH), bc)
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
	// BackgroundColor is the base cef_color_t (CEF_COLOR_ARGB layout:
	// 0xAARRGGBB) applied before the first paint; 0 keeps CEF's default.
	BackgroundColor uint32
}

var closingBrowsers atomic.Bool

// CloseAllBrowsers force-closes every live browser and waits until their
// before_close notifications arrive (CEF requires this before shutdown),
// bounded by timeout. Returns the number of browsers still alive.
func CloseAllBrowsers(timeout time.Duration) int {
	closingBrowsers.Store(true)
	var browsers []*Browser
	browsersByID.Range(func(_, v any) bool {
		browsers = append(browsers, v.(*Browser))
		return true
	})
	pendingBrowsers.Range(func(_, v any) bool {
		v.(*Browser).Close(true)
		return true
	})
	for _, b := range browsers {
		b.Close(true)
	}
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		// Native child destruction and queued host callbacks also need GLib.
		if st := state.Load(); st != nil && st.PumpHostLoop != nil {
			st.PumpHostLoop()
		}
		// The GTK loop has stopped; close notifications still need the CEF UI pump.
		C.wcef_do_message_loop_work()
		alive := 0
		browsersByID.Range(func(_, _ any) bool {
			alive++
			return true
		})
		pendingBrowsers.Range(func(_, _ any) bool { alive++; return true })
		if alive == 0 {
			return 0
		}
		time.Sleep(50 * time.Millisecond)
	}
	alive := 0
	browsersByID.Range(func(_, _ any) bool {
		alive++
		return true
	})
	pendingBrowsers.Range(func(_, _ any) bool { alive++; return true })
	return alive
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
	C.wcef_window_parent(wi, C.uintptr_t(opts.ParentXWindow))
	wi.windowless_rendering_enabled = 0
	wi.runtime_style = C.CEF_RUNTIME_STYLE_ALLOY
	defer C.free(unsafe.Pointer(wi))

	settings := (*C.cef_browser_settings_t)(C.calloc(1, C.sizeof_cef_browser_settings_t))
	settings.size = C.sizeof_cef_browser_settings_t
	if opts.BackgroundColor != 0 {
		settings.background_color = C.cef_color_t(opts.BackgroundColor)
	}
	defer C.free(unsafe.Pointer(settings))

	url := newCefString(opts.URL)
	defer url.Clear()

	// The pending browser is completed by on_after_created (see
	// lifeSpanAfterCreated); associate the client with the window now so
	// the mapping exists the moment the first scheme request arrives.
	b := &Browser{windowID: opts.WindowID, client: bc}
	pendingBrowsers.Store(bc, b)

	if C.wcef_create_browser(wi, bc.c, url.ptr(), settings) != 1 {
		pendingBrowsers.Delete(bc)
		return nil, errCreateBrowser
	}
	return b, nil
}

var (
	pendingBrowsers sync.Map // *browserClient -> *Browser (until on_after_created)
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
	b := &Browser{client: client}
	if pending, ok := pendingBrowsers.LoadAndDelete(client); ok {
		// The GTK engine retains the pointer returned by CreateBrowser.
		// Complete that same object, otherwise all host operations see nil.
		b = pending.(*Browser)
		browsersByWin.Store(client.windowID, b)
	}
	b.c = browser
	b.host = C.wcef_browser_get_host(browser)
	browsersByID.Store(int(C.wcef_browser_get_identifier(browser)), b)
	if b.closeRequested || closingBrowsers.Load() {
		b.Close(true)
	}
}

// lifeSpanDoClose implements cef_life_span_handler_t.do_close: let CEF
// close the browser (and its native child window) normally.
func lifeSpanDoClose(browser *C.cef_browser_t, client *browserClient) C.int {
	if value, ok := browsersByID.Load(int(C.wcef_browser_get_identifier(browser))); ok {
		b := value.(*Browser)
		if primary, ok := browsersByWin.Load(b.windowID); ok && primary == b {
			if st := state.Load(); st != nil && st.OnBrowserClosing != nil {
				return gtkBoolC(st.OnBrowserClosing(b.windowID))
			}
		}
	}
	// Linux lets CEF close its X11 child; native desktop hosts must instead
	// destroy the top-level window once DoClose permits it (CEF contract).
	return 0
}

// lifeSpanBeforeClose implements cef_life_span_handler_t.on_before_close.
func lifeSpanBeforeClose(browser *C.cef_browser_t, client *browserClient) {
	id := int(C.wcef_browser_get_identifier(browser))
	primary := false
	windowID := uint(0)
	if b, ok := browsersByID.LoadAndDelete(id); ok {
		browser := b.(*Browser)
		primary = browsersByWin.CompareAndDelete(browser.windowID, browser)
		windowID = browser.windowID
		C.wcef_obj_release(unsafe.Pointer(browser.host))
		C.wcef_obj_release(unsafe.Pointer(browser.c))
		browser.host = nil
		browser.c = nil
	}
	st := state.Load()
	if primary && st != nil && st.OnBrowserClosed != nil {
		st.OnBrowserClosed(windowID)
	}
}

// loadStart implements cef_load_handler_t.on_load_start for the main
// frame: maps to the wails WindowLoadStarted event.
func loadStart(browser *C.cef_browser_t, frame *C.cef_frame_t, client *browserClient) {
	if frame == nil || C.wcef_frame_is_main(frame) != 1 {
		return
	}
	st := state.Load()
	if st != nil && st.OnWindowLoadStart != nil {
		st.OnWindowLoadStart(client.windowID)
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

// renderCrash handles a renderer death for one browser: notifies the
// host and schedules exactly one automatic reload of the main frame so
// a crashed page returns to its URL. Repeated crashes within the cool
// down are surfaced to the host without further automatic attempts, so
// a permanently crashing page cannot create an infinite reload loop.
func renderCrash(browser *C.cef_browser_t, status int, client *browserClient) {
	id := int(C.wcef_browser_get_identifier(browser))
	b := lookupBrowserByCefID(id)
	if b == nil {
		return
	}
	now := time.Now()
	b.crashMu.Lock()
	recovered := false
	if now.Sub(b.lastCrash) > crashCoolDown {
		b.lastCrash = now
		recovered = true
	}
	b.crashMu.Unlock()
	if recovered {
		// Reload runs on CEF's UI thread via the host pump, not inside
		// this termination callback (CEF forbids re-entry here).
		st := state.Load()
		if st != nil && st.DispatchMain != nil {
			url := b.currentURL()
			st.DispatchMain(func() {
				if bb := lookupBrowserByCefID(id); bb != nil {
					bb.Reload(true)
					_ = url
				}
			})
		}
	}
	st := state.Load()
	if st != nil && st.OnRenderCrash != nil {
		st.OnRenderCrash(client.windowID, status)
	}
}

// crashCoolDown bounds automatic reload: one attempt per interval.
const crashCoolDown = 10 * time.Second

// currentURL snapshots the main frame URL for recovery logging.
func (b *Browser) currentURL() string {
	if b.c == nil || C.wcef_browser_is_valid(b.c) != 1 {
		return ""
	}
	frame := C.wcef_browser_get_main_frame(b.c)
	if frame == nil {
		return ""
	}
	return userfreeToString(C.wcef_frame_get_url(frame))
}

// keyEvent implements cef_keyboard_handler_t.on_key_event: forwards raw
// keydowns to the host (accelerators, key bindings) and lets the host
// decide consumption.
func keyEvent(browser *C.cef_browser_t, event *C.cef_key_event_t, client *browserClient) C.int {
	if event == nil {
		return 0
	}
	if event._type != C.KEYEVENT_RAWKEYDOWN && event._type != C.KEYEVENT_KEYDOWN {
		return 0
	}
	st := state.Load()
	if st == nil || st.OnKeyEvent == nil {
		return 0
	}
	key := uint32(event.native_key_code)
	if runtime.GOOS != "linux" {
		key = uint32(event.windows_key_code)
	}
	consumed := st.OnKeyEvent(client.windowID, key, uint32(event.modifiers))
	if consumed {
		return 1
	}
	return 0
}

// mediaPermission implements
// cef_permission_handler_t.on_request_media_access_permission, honouring
// the host's decision (mirrors the system webview permission handling).
func mediaPermission(browser *C.cef_browser_t, origin *C.cef_string_t, requestedPermissions C.uint32_t, callback unsafe.Pointer, client *browserClient) C.int {
	st := state.Load()
	if st == nil || st.OnMediaPermission == nil {
		return 0
	}
	devicePermissions := C.uint32_t(C.CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE | C.CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE)
	if requestedPermissions & ^devicePermissions != 0 {
		// Camera/microphone policy does not authorize screen or system-audio capture.
		C.wcef_media_callback_cont(callback, 0)
		return 1
	}
	needVideo := requestedPermissions&C.uint32_t(C.CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE) != 0
	needAudio := requestedPermissions&C.uint32_t(C.CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE) != 0
	if st.OnMediaPermission(client.windowID, needAudio, needVideo) {
		if runtime.GOOS == "darwin" && origin != nil {
			// macOS permission observers can otherwise report ASK immediately
			// after the explicit grant and cancel the newly opened capture stream.
			C.wcef_record_media_permission(browser, origin, requestedPermissions)
		}
		C.wcef_media_callback_cont(callback, requestedPermissions)
	} else {
		C.wcef_media_callback_cont(callback, 0)
	}
	return 1
}

// dragEnter tracks file drags for navigation prevention. GTK handles
// external drops and routes their coordinates to the runtime.
func dragEnter(browser *C.cef_browser_t, dragData *C.cef_drag_data_t, client *browserClient) C.int {
	client.dragMu.Lock()
	client.dragPending = nil
	client.dragMu.Unlock()
	if dragData == nil {
		return 0
	}
	list := C.wcef_drag_data_get_file_paths(dragData)
	defer C.wcef_string_list_free(list)
	n := int(C.wcef_string_list_size(list))
	if n == 0 {
		return 0
	}
	var files []string
	var out C.cef_string_t
	for i := 0; i < n; i++ {
		if C.wcef_string_list_value(list, C.size_t(i), &out) != 1 {
			continue
		}
		files = append(files, goString(&out))
		C.wcef_string_utf16_clear(&out)
	}
	if len(files) == 0 {
		return 0
	}
	client.dragMu.Lock()
	client.dragPending = files
	client.dragMu.Unlock()
	// Keep Chromium HTML drag handling; block any file navigation below.
	return 0
}

// beforeBrowse cancels file navigation caused by native file drags.
func beforeBrowse(browser *C.cef_browser_t, request *C.cef_request_t, client *browserClient) C.int {
	if request == nil {
		return 0
	}
	url := userfreeToString(C.wcef_request_get_url(request))
	if !strings.HasPrefix(url, "file://") {
		return 0
	}
	client.dragMu.Lock()
	files := client.dragPending
	client.dragPending = nil
	client.dragMu.Unlock()
	if len(files) == 0 {
		return 0
	}
	// A native file drag must never navigate away from the app.
	// Actual drops are delivered by the GTK destination.
	return 1 // cancel the file:// navigation
}

// ExecJS evaluates JavaScript in the browser's main frame. CEF marshals
// execution onto its UI thread internally.
func (b *Browser) ExecJS(js string) {
	if b == nil || b.c == nil || C.wcef_browser_is_valid(b.c) != 1 {
		return
	}
	frame := C.wcef_browser_get_main_frame(b.c)
	if frame == nil {
		return
	}
	defer C.wcef_obj_release(unsafe.Pointer(frame))
	if C.wcef_frame_is_valid(frame) != 1 {
		return
	}
	code := newCefString(js)
	defer code.Clear()
	url := newCefString(AssetOrigin + "/wails/exec")
	defer url.Clear()
	C.wcef_frame_exec_js(frame, code.ptr(), url.ptr(), 1)
}

// LoadURL navigates the main frame to the given URL.
func (b *Browser) LoadURL(url string) {
	if b == nil || b.c == nil || C.wcef_browser_is_valid(b.c) != 1 {
		return
	}
	frame := C.wcef_browser_get_main_frame(b.c)
	if frame == nil {
		return
	}
	defer C.wcef_obj_release(unsafe.Pointer(frame))
	if C.wcef_frame_is_valid(frame) != 1 {
		return
	}
	u := newCefString(url)
	defer u.Clear()
	C.wcef_frame_load_url(frame, u.ptr())
}

// Reload reloads the page, optionally bypassing the cache.
func (b *Browser) Reload(ignoreCache bool) {
	if b == nil || b.c == nil || C.wcef_browser_is_valid(b.c) != 1 {
		return
	}
	if ignoreCache {
		C.wcef_browser_reload_ignore_cache(b.c)
	} else {
		C.wcef_browser_reload(b.c)
	}
}

// SetZoomFactor maps a wails zoom factor (1.0 = 100%) onto CEF's
// logarithmic zoom level (level 0 = 100%).
func (b *Browser) SetZoomFactor(factor float64) {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_set_zoom_level(b.host, C.double(math.Log2(factor)))
}

// ZoomFactor returns the current zoom as a wails zoom factor.
func (b *Browser) ZoomFactor() float64 {
	if b == nil || b.host == nil {
		return 1
	}
	return math.Pow(2, float64(C.wcef_host_get_zoom_level(b.host)))
}

// OpenDevTools opens CEF's developer tools window (default position).
func (b *Browser) OpenDevTools() {
	if b == nil || b.host == nil {
		return
	}
	if b.devToolsClient == nil {
		b.devToolsClient = newBrowserClient(0)
		clients.Store(unsafe.Pointer(b.devToolsClient.c), b.devToolsClient)
	}
	C.wcef_host_show_dev_tools(b.host, b.devToolsClient.c)
}

// Close destroys the browser. The native child window is removed
// asynchronously; OnBrowserClosed fires when it completes.
func (b *Browser) Close(force bool) {
	if b == nil {
		return
	}
	b.closeRequested = true
	if b.host == nil {
		return
	}
	C.wcef_browser_stop_load(b.c)
	if b.devToolsClient != nil {
		C.wcef_host_close_dev_tools(b.host)
	}
	C.wcef_host_close_browser(b.host, gtkBoolC(force))
}

// Focus notifies Chromium and focuses its native embedded child.
func (b *Browser) Focus() {
	if b != nil && b.host != nil {
		C.wcef_host_set_focus(b.host)
	}
}

// XWindow returns the browser's native X11 window handle (0 while the
// browser has not created it yet).
func (b *Browser) XWindow() uintptr {
	if b == nil || b.host == nil {
		return 0
	}
	return uintptr(C.wcef_native_handle(b.host))
}

func gtkBoolC(v bool) C.int {
	if v {
		return 1
	}
	return 0
}
