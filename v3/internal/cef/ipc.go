//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}
*/
import "C"

// IPC message names exchanged between the renderer V8 extension and the
// browser process.
const (
	ipcMessageInvoke = "wails.invoke"
	v8FunctionName   = "wailsInvoke"
	extensionName    = "wails"
)

// v8ExtensionSource is registered in every renderer at WebKit init. It
// exposes window.wails.invoke(msg) — the exact surface the wails runtime
// already uses on Android (see @wailsio/runtime system.ts), so the
// frontend needs no CEF-specific code path.
const v8ExtensionSource = `
var wails;
if (!wails) { wails = {}; }
(function() {
  native function wailsInvoke();
  wails.invoke = function(msg) {
    if (typeof msg !== 'string') { msg = JSON.stringify(msg); }
    wailsInvoke(msg);
  };
})();
`

// registerWailsV8Extension installs the window.wails.invoke extension.
// Runs in every renderer process (renderWebKitInitialized).
func registerWailsV8Extension() {
	name := newCefString(extensionName)
	defer name.Clear()
	code := newCefString(v8ExtensionSource)
	defer code.Clear()
	C.wcef_register_extension(name.ptr(), code.ptr(), theApp.v8)
}

// v8Execute implements cef_v8_handler_t.execute in the renderer process:
// forwards window.wails.invoke payloads to the browser process via a
// frame process message.
func v8Execute(name *C.cef_string_t, object *C.cef_v8_value_t, argumentsCount C.size_t, arguments **C.cef_v8_value_t, retval **C.cef_v8_value_t, exception *C.cef_string_t) C.int {
	if goString(name) != v8FunctionName {
		return 0
	}
	if argumentsCount < 1 || arguments == nil {
		return 0
	}
	arg0 := *arguments
	if arg0 == nil || C.wcef_v8v_is_string(arg0) != 1 {
		return 0
	}
	msg := userfreeToString(C.wcef_v8v_get_string_value(arg0))

	frame := currentFrame()
	if frame == nil {
		return 0
	}

	msgName := newCefString(ipcMessageInvoke)
	defer msgName.Clear()
	m := C.wcef_process_message_create(msgName.ptr(), C.PID_BROWSER)
	if m == nil {
		return 0
	}
	args := C.wcef_msg_get_argument_list(m)
	str := newCefString(msg)
	defer str.Clear()
	C.wcef_list_set_string(args, 0, str.ptr())
	C.wcef_frame_send_message(frame, C.PID_BROWSER, m)
	return 1
}

// currentFrame resolves the frame the executing script belongs to.
func currentFrame() *C.cef_frame_t {
	ctx := C.wcef_v8_context_get_current()
	if ctx == nil {
		return nil
	}
	return C.wcef_v8ctx_get_frame(ctx)
}

// browserProcessMessage implements cef_client_t.on_process_message_received
// in the browser process: dispatches wails.invoke payloads into the wails
// window message pipeline.
func browserProcessMessage(browser *C.cef_browser_t, frame *C.cef_frame_t, sourceProcess C.cef_process_id_t, message *C.cef_process_message_t) C.int {
	if sourceProcess != C.PID_RENDERER {
		return 0
	}
	name := userfreeToString(C.wcef_msg_get_name(message))
	if name != ipcMessageInvoke {
		return 0
	}

	args := C.wcef_msg_get_argument_list(message)
	if args == nil || C.wcef_list_get_size(args) < 1 {
		return 1
	}
	msg := userfreeToString(C.wcef_list_get_string(args, 0))

	var origin string
	if frame != nil {
		origin = userfreeToString(C.wcef_frame_get_url(frame))
	}

	st := state.Load()
	b := lookupBrowserByCefID(int(C.wcef_browser_get_identifier(browser)))
	if st != nil && st.OnWindowMessage != nil && b != nil {
		st.OnWindowMessage(b.windowID, msg, origin)
	}
	return 1
}
