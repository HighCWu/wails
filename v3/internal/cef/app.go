//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}

// Shims: thin C wrappers over the //export'ed Go callbacks in
// handlers.go, with prototypes exactly matching the CEF struct fields.

// cef_app_t
void wailsCEFAppRegisterSchemes(struct _cef_app_t* self, struct _cef_scheme_registrar_t* registrar);
static void wails_cef_app_register_schemes(struct _cef_app_t* self, struct _cef_scheme_registrar_t* registrar) {
  wailsCEFAppRegisterSchemes(self, registrar);
}
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
void wailsCEFRPHWebKitInitialized(struct _cef_render_process_handler_t* self);
static void wails_cef_rph_webkit_initialized(struct _cef_render_process_handler_t* self) {
  wailsCEFRPHWebKitInitialized(self);
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
  a->on_register_custom_schemes = wails_cef_app_register_schemes;
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
  h->on_web_kit_initialized = wails_cef_rph_webkit_initialized;
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

// SchemeName is the custom scheme the asset server is served under,
// matching the system webview backends (wails://localhost/...).
const SchemeName = "wails"

// schemeOptions mirrors what the WebKit backends get by registering the
// scheme: a standard, secure, fetchable scheme with CORS support so the
// runtime's default HTTP fetch transport works against it.
const schemeOptions = C.CEF_SCHEME_OPTION_STANDARD |
	C.CEF_SCHEME_OPTION_SECURE |
	C.CEF_SCHEME_OPTION_CORS_ENABLED |
	C.CEF_SCHEME_OPTION_FETCH_ENABLED

// appRegisterSchemes implements cef_app_t.on_register_custom_schemes,
// invoked in every process before command line processing.
func appRegisterSchemes(registrar *C.cef_scheme_registrar_t) {
	scheme := newCefString(SchemeName)
	defer scheme.Clear()
	C.wcef_registrar_add_custom_scheme(registrar, scheme.ptr(), schemeOptions)
}

// appOnCommandLine implements cef_app_t.on_before_command_line_processing:
// appends the switches the Go runtime and unsandboxed layout require.
func appOnCommandLine(processType *C.cef_string_t, commandLine *C.cef_command_line_t) {
	if processType != nil && goString(processType) != "" {
		// Subprocess command lines are derived from the browser process
		// one; switches below are inherited automatically.
		return
	}
	appendSwitch := func(name string) {
		s := newCefString(name)
		defer s.Clear()
		C.wcef_cl_append_switch(commandLine, s.ptr())
	}
	// The Go runtime cannot serve as a zygote host.
	appendSwitch("no-zygote")
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

// renderWebKitInitialized implements
// cef_render_process_handler_t.on_web_kit_initialized: installs the
// window.wails.invoke V8 extension in every renderer.
func renderWebKitInitialized() {
	registerWailsV8Extension()
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

// registerSchemeFactory wires wails:// requests to the factory in
// scheme.go. Called from Initialize (browser process only).
func registerSchemeFactory() bool {
	if !Loaded() {
		return false
	}
	factory := buildSchemeFactory()
	scheme := newCefString(SchemeName)
	defer scheme.Clear()
	return C.wcef_register_scheme_handler_factory(scheme.ptr(), nil, factory) == 1
}
