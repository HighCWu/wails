//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15400

// Shims: thin C wrappers over the //export'ed Go callbacks in
// handlers.go, with prototypes exactly matching the CEF struct fields.

// cef_app_t
void wailsCEFAppOnCommandLine(struct _cef_app_t* self, const cef_string_t* process_type, struct _cef_command_line_t* command_line);
static void wails_cef_app_on_command_line(struct _cef_app_t* self, const cef_string_t* process_type, struct _cef_command_line_t* command_line) {
  wailsCEFAppOnCommandLine(self, process_type, command_line);
}
struct _cef_browser_process_handler_t* wailsCEFAppGetBPH(struct _cef_app_t* self);
static struct _cef_browser_process_handler_t* wails_cef_app_get_bph(struct _cef_app_t* self) {
  return wailsCEFAppGetBPH(self);
}
struct _cef_render_process_handler_t* wailsCEFAppGetRPH(struct _cef_app_t* self);
static struct _cef_render_process_handler_t* wails_cef_app_get_rph(struct _cef_app_t* self) {
  return wailsCEFAppGetRPH(self);
}

// cef_browser_process_handler_t
void wailsCEFBPHContextInitialized(struct _cef_browser_process_handler_t* self);
static void wails_cef_bph_context_initialized(struct _cef_browser_process_handler_t* self) {
  wailsCEFBPHContextInitialized(self);
}

// cef_render_process_handler_t
void wailsCEFRPHContextCreated(struct _cef_render_process_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, struct _cef_v8_context_t* context);
static void wails_cef_rph_context_created(struct _cef_render_process_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, struct _cef_v8_context_t* context) {
  wailsCEFRPHContextCreated(self, browser, frame, context);
}
int wailsCEFRPHProcessMessage(struct _cef_render_process_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, cef_process_id_t source_process, struct _cef_process_message_t* message);
static int wails_cef_rph_process_message(struct _cef_render_process_handler_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, cef_process_id_t source_process, struct _cef_process_message_t* message) {
  return wailsCEFRPHProcessMessage(self, browser, frame, source_process, message);
}

// cef_v8_handler_t
int wailsCEFV8Execute(struct _cef_v8_handler_t* self, const cef_string_t* name, struct _cef_v8_value_t* object, size_t arguments_count, struct _cef_v8_value_t** arguments, struct _cef_v8_value_t** retval, cef_string_t* exception);
static int wails_cef_v8_execute(struct _cef_v8_handler_t* self, const cef_string_t* name, struct _cef_v8_value_t* object, size_t arguments_count, struct _cef_v8_value_t* const* arguments, struct _cef_v8_value_t** retval, cef_string_t* exception) {
  return wailsCEFV8Execute(self, name, object, arguments_count, (struct _cef_v8_value_t**)arguments, retval, exception);
}

// Struct initialisers: cgo exposes function-pointer struct fields as
// *[0]byte, so callback fields must be wired from C.
static void wcef_init_app(void* p) {
  cef_app_t* a = (cef_app_t*)p;
  a->on_before_command_line_processing = wails_cef_app_on_command_line;
  a->get_browser_process_handler = wails_cef_app_get_bph;
  a->get_render_process_handler = wails_cef_app_get_rph;
}
static void wcef_init_bph(void* p) {
  cef_browser_process_handler_t* h = (cef_browser_process_handler_t*)p;
  h->on_context_initialized = wails_cef_bph_context_initialized;
}
static void wcef_init_rph(void* p) {
  cef_render_process_handler_t* h = (cef_render_process_handler_t*)p;
  h->on_context_created = wails_cef_rph_context_created;
  h->on_process_message_received = wails_cef_rph_process_message;
}
static void wcef_init_v8handler(void* p) {
  cef_v8_handler_t* h = (cef_v8_handler_t*)p;
  h->execute = wails_cef_v8_execute;
}
*/
import "C"

import (
	"os"
	"strings"
	"unsafe"
)

// app holds every CEF struct assembled for process bootstrap. The Go
// values keep the C allocations registered (and thus refcounted/freed by
// the callbacks in callbacks.go) for the life of the process.
type app struct {
	app *C.cef_app_t
	bph *C.cef_browser_process_handler_t
	rph *C.cef_render_process_handler_t
	v8  *C.cef_v8_handler_t
}

var theApp *app

// buildApp assembles the cef_app_t (with browser- and render-process
// handlers) reused for both the browser process and every subprocess.
func buildApp() *C.cef_app_t {
	if theApp != nil {
		return theApp.app
	}

	bph := (*C.cef_browser_process_handler_t)(allocStruct(C.sizeof_cef_browser_process_handler_t))
	C.wcef_init_bph(unsafe.Pointer(bph))

	rph := (*C.cef_render_process_handler_t)(allocStruct(C.sizeof_cef_render_process_handler_t))
	C.wcef_init_rph(unsafe.Pointer(rph))

	v8 := (*C.cef_v8_handler_t)(allocStruct(C.sizeof_cef_v8_handler_t))
	C.wcef_init_v8handler(unsafe.Pointer(v8))

	a := (*C.cef_app_t)(allocStruct(C.sizeof_cef_app_t))
	C.wcef_init_app(unsafe.Pointer(a))

	theApp = &app{app: a, bph: bph, rph: rph, v8: v8}
	return a
}

// AssetScheme and AssetHost form the origin the CEF backend serves the
// asset server from. It deliberately matches the Windows WebView2 backend
// (http://wails.localhost) instead of the Linux WebKit backends' custom
// wails:// scheme: Chromium restricts non-special schemes, while a
// domain-scoped http factory behaves exactly like any web origin for
// fetch/XHR/CORS.
const (
	AssetScheme = "http"
	AssetHost   = "wails.localhost"
)

// AssetOrigin is the URL prefix serving the wails asset server.
const AssetOrigin = "http://" + AssetHost

// appOnCommandLine implements cef_app_t.on_before_command_line_processing:
// appends the switches the Go runtime and unsandboxed layout require.
func appOnCommandLine(processType *C.cef_string_t, commandLine *C.cef_command_line_t) {
	if processType != nil && goString(processType) != "" {
		// Subprocess command lines are derived from the browser process
		// one; switches below are inherited automatically.
		return
	}
	appendSwitch := func(sw string) {
		name, value, hasValue := strings.Cut(sw, "=")
		s := newCefString(name)
		defer s.Clear()
		if hasValue {
			v := newCefString(value)
			defer v.Clear()
			C.wcef_cl_append_switch_value(commandLine, s.ptr(), v.ptr())
		} else {
			C.wcef_cl_append_switch(commandLine, s.ptr())
		}
	}
	// The Go runtime cannot serve as a zygote host.
	appendSwitch("no-zygote")
	// Embedded webviews are managed by the host toolkit: Chromium's
	// native-window occlusion detection misjudges focus/stack changes on
	// X11 (worst on compositors without a real screen, e.g. Xvfb) and
	// stops painting, leaving a black window. Disable it.
	appendSwitch("disable-backgrounding-occluded-windows")
	// Chromium 132+ shows a first-run Terms-of-Service dialog on fresh
	// profiles on Linux; an embedded webview must never do that.
	appendSwitch("no-first-run")
	appendSwitch("no-default-browser-check")
	appendSwitch("disable-features=CalculateNativeWinOcclusion")
	if initializeOptions.EnableSandbox {
		// nothing extra; the sandbox helper must be setuid root
	} else {
		appendSwitch("no-sandbox")
	}
	for _, sw := range extraSwitchesFromEnv() {
		appendSwitch(sw)
	}
	for _, sw := range initializeOptions.ExtraSwitches {
		appendSwitch(sw)
	}
}

func extraSwitchesFromEnv() []string {
	raw := os.Getenv("WAILS_CEF_SWITCHES")
	if raw == "" {
		return nil
	}
	var switches []string
	for _, s := range strings.Split(raw, ",") {
		if s = strings.TrimSpace(s); s != "" {
			switches = append(switches, s)
		}
	}
	return switches
}

// browserProcessContextInitialized implements
// cef_browser_process_handler_t.on_context_initialized. The scheme
// factory registration happens right after cef_initialize instead (any
// browser-process thread is documented as acceptable); keep this hook for
// future early-singleton work.
func browserProcessContextInitialized() {}

// renderContextCreated implements
// cef_render_process_handler_t.on_context_created: installs the
// window.wails.invoke binding in every JS context (the replacement for
// CefRegisterExtension, which was removed in CEF API 15400).
func renderContextCreated(context *C.cef_v8_context_t) {
	installWailsV8Binding(context)
}

// renderProcessMessage implements
// cef_render_process_handler_t.on_process_message_received. The wails
// bridge is render→browser only; nothing is expected here yet.
func renderProcessMessage(browser *C.cef_browser_t, frame *C.cef_frame_t, sourceProcess C.cef_process_id_t, message *C.cef_process_message_t) C.int {
	_ = browser
	_ = frame
	_ = sourceProcess
	_ = message
	return 0
}

// registerSchemeFactory wires http://wails.localhost requests to the
// factory in scheme.go. Called from Initialize (browser process only).
func registerSchemeFactory() bool {
	if !Loaded() {
		return false
	}
	factory := buildSchemeFactory()
	scheme := newCefString(AssetScheme)
	defer scheme.Clear()
	domain := newCefString(AssetHost)
	defer domain.Clear()
	return C.wcef_register_scheme_handler_factory(scheme.ptr(), domain.ptr(), factory) == 1
}
