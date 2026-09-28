//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15400
*/
import "C"

import (
	"strconv"
	"strings"
	"unsafe"
)

// IPC message names exchanged between the renderer binding and the
// browser process.
const (
	ipcMessageInvoke = "wails.invoke"
	v8FunctionName   = "invoke"
)

// installWailsV8Binding exposes window.wails.invoke(msg) in a JS context —
// the exact surface the wails runtime already uses on Android (see
// @wailsio/runtime system.ts), so the frontend needs no CEF-specific code
// path. Runs from on_context_created in every renderer context.
func installWailsV8Binding(context *C.cef_v8_context_t) {
	if context == nil || theApp == nil {
		return
	}
	global := C.wcef_v8ctx_get_global(context)
	if global == nil {
		return
	}
	defer C.wcef_obj_release(unsafe.Pointer(global))
	obj := C.wcef_v8_value_create_object()
	if obj == nil {
		return
	}
	defer C.wcef_obj_release(unsafe.Pointer(obj))
	name := newCefString(v8FunctionName)
	defer name.Clear()
	fn := C.wcef_v8_value_create_function(name.ptr(), theApp.v8)
	if fn == nil {
		return
	}
	defer C.wcef_obj_release(unsafe.Pointer(fn))
	if C.wcef_v8_value_set_bykey(obj, name.ptr(), fn) != 1 {
		return
	}
	wailsKey := newCefString("wails")
	defer wailsKey.Clear()
	C.wcef_v8_value_set_bykey(global, wailsKey.ptr(), obj)
	C.wcef_install_webview_bridge(global, fn)
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
	defer C.wcef_obj_release(unsafe.Pointer(frame))

	msgName := newCefString(ipcMessageInvoke)
	defer msgName.Clear()
	m := C.wcef_process_message_create(msgName.ptr(), C.PID_BROWSER)
	if m == nil {
		return 0
	}
	args := C.wcef_msg_get_argument_list(m)
	defer C.wcef_obj_release(unsafe.Pointer(args))
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
	defer C.wcef_obj_release(unsafe.Pointer(ctx))
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
	if args == nil {
		return 1
	}
	defer C.wcef_obj_release(unsafe.Pointer(args))
	if C.wcef_list_get_size(args) < 1 {
		return 1
	}
	msg := userfreeToString(C.wcef_list_get_string(args, 0))

	var origin string
	if frame != nil {
		origin = userfreeToString(C.wcef_frame_get_url(frame))
	}

	st := state.Load()
	b := lookupBrowserByCefID(int(C.wcef_browser_get_identifier(browser)))
	if st != nil && b != nil && strings.HasPrefix(msg, "file:drop:") && st.OnFileDrop != nil {
		parts := strings.Split(msg, ":")
		if len(parts) == 4 && frame != nil && C.wcef_frame_is_main(frame) == 1 {
			x, xe := strconv.Atoi(parts[2])
			y, ye := strconv.Atoi(parts[3])
			if xe == nil && ye == nil {
				b.client.dragMu.Lock()
				files := b.client.dragPending
				b.client.dragPending = nil
				b.client.dragMu.Unlock()
				if len(files) > 0 {
					st.OnFileDrop(b.windowID, files, x, y)
				}
			}
		}
		return 1
	}
	if st != nil && st.OnWindowMessage != nil && b != nil {
		st.OnWindowMessage(b.windowID, msg, origin)
	}
	return 1
}
