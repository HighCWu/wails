//go:build linux && wails_cef

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15200
*/
import "C"

import "unsafe"

// Exported C callbacks (invoked by CEF through the static shims declared
// in each area file's preamble). Every function must keep its preamble
// declaration-only; the Go bodies live here and delegate to unexported
// handlers next to the code that owns the struct.

// --- cef_app_t ---

//export wailsCEFAppOnCommandLine
func wailsCEFAppOnCommandLine(self *C.cef_app_t, processType *C.cef_string_t, commandLine *C.cef_command_line_t) {
	appOnCommandLine(processType, commandLine)
}

//export wailsCEFAppGetBPH
func wailsCEFAppGetBPH(self *C.cef_app_t) *C.cef_browser_process_handler_t {
	return theApp.bph
}

//export wailsCEFAppGetRPH
func wailsCEFAppGetRPH(self *C.cef_app_t) *C.cef_render_process_handler_t {
	return theApp.rph
}

// --- cef_browser_process_handler_t ---

//export wailsCEFBPHContextInitialized
func wailsCEFBPHContextInitialized(self *C.cef_browser_process_handler_t) {
	browserProcessContextInitialized()
}

// --- cef_render_process_handler_t ---

//export wailsCEFRPHContextCreated
func wailsCEFRPHContextCreated(self *C.cef_render_process_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, context *C.cef_v8_context_t) {
	renderContextCreated(context)
}

//export wailsCEFRPHProcessMessage
func wailsCEFRPHProcessMessage(self *C.cef_render_process_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, sourceProcess C.cef_process_id_t, message *C.cef_process_message_t) C.int {
	return renderProcessMessage(browser, frame, sourceProcess, message)
}

// --- cef_v8_handler_t (render process) ---

//export wailsCEFV8Execute
func wailsCEFV8Execute(self *C.cef_v8_handler_t, name *C.cef_string_t, object *C.cef_v8_value_t, argumentsCount C.size_t, arguments **C.cef_v8_value_t, retval **C.cef_v8_value_t, exception *C.cef_string_t) C.int {
	return v8Execute(name, object, argumentsCount, arguments, retval, exception)
}

// --- cef_scheme_handler_factory_t ---

//export wailsCEFFactoryCreate
func wailsCEFFactoryCreate(self *C.cef_scheme_handler_factory_t, browser *C.cef_browser_t, frame *C.cef_frame_t, schemeName *C.cef_string_t, request *C.cef_request_t) *C.cef_resource_handler_t {
	return factoryCreate(browser, frame, schemeName, request)
}

// --- cef_resource_handler_t ---

//export wailsCEFResourceProcessRequest
func wailsCEFResourceProcessRequest(self *C.cef_resource_handler_t, request *C.cef_request_t, callback *C.cef_callback_t) C.int {
	return resourceProcessRequest(self, request, callback)
}

//export wailsCEFResourceGetResponseHeaders
func wailsCEFResourceGetResponseHeaders(self *C.cef_resource_handler_t, response *C.cef_response_t, responseLength *C.int64_t, redirectURL *C.cef_string_t) {
	resourceGetResponseHeaders(self, response, responseLength, redirectURL)
}

//export wailsCEFResourceReadResponse
func wailsCEFResourceReadResponse(self *C.cef_resource_handler_t, dataOut unsafe.Pointer, bytesToRead C.int, bytesRead *C.int, callback *C.cef_callback_t) C.int {
	return resourceReadResponse(self, dataOut, bytesToRead, bytesRead, callback)
}

//export wailsCEFResourceCancel
func wailsCEFResourceCancel(self *C.cef_resource_handler_t) {
	resourceCancel(self)
}

// --- cef_client_t ---

//export wailsCEFClientGetLSH
func wailsCEFClientGetLSH(self *C.cef_client_t) *C.cef_life_span_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.lsh
	}
	return nil
}

//export wailsCEFClientGetLoadH
func wailsCEFClientGetLoadH(self *C.cef_client_t) *C.cef_load_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.loadH
	}
	return nil
}

//export wailsCEFClientGetDisplayH
func wailsCEFClientGetDisplayH(self *C.cef_client_t) *C.cef_display_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.displayH
	}
	return nil
}

//export wailsCEFClientProcessMessage
func wailsCEFClientProcessMessage(self *C.cef_client_t, browser *C.cef_browser_t, frame *C.cef_frame_t, sourceProcess C.cef_process_id_t, message *C.cef_process_message_t) C.int {
	return browserProcessMessage(browser, frame, sourceProcess, message)
}

// --- cef_life_span_handler_t ---

//export wailsCEFLSHAfterCreated
func wailsCEFLSHAfterCreated(self *C.cef_life_span_handler_t, browser *C.cef_browser_t) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		lifeSpanAfterCreated(browser, bc)
	}
}

//export wailsCEFLSHDoClose
func wailsCEFLSHDoClose(self *C.cef_life_span_handler_t, browser *C.cef_browser_t) C.int {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		return lifeSpanDoClose(browser, bc)
	}
	return 0
}

//export wailsCEFLSHBeforeClose
func wailsCEFLSHBeforeClose(self *C.cef_life_span_handler_t, browser *C.cef_browser_t) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		lifeSpanBeforeClose(browser, bc)
	}
}

// --- cef_load_handler_t ---

//export wailsCEFLoadHOnLoadStart
func wailsCEFLoadHOnLoadStart(self *C.cef_load_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, transitionType C.int) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		loadStart(browser, frame, bc)
	}
}

//export wailsCEFLoadHOnLoadEnd
func wailsCEFLoadHOnLoadEnd(self *C.cef_load_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, httpCode C.int) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		loadEnd(browser, frame, int(httpCode), bc)
	}
}

//export wailsCEFLoadHOnLoadError
func wailsCEFLoadHOnLoadError(self *C.cef_load_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, errorCode C.int, errorText *C.cef_string_t, failedURL *C.cef_string_t) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		loadError(browser, frame, int(errorCode), errorText, failedURL, bc)
	}
}

// --- cef_display_handler_t ---

//export wailsCEFClientGetKeyboardH
func wailsCEFClientGetKeyboardH(self *C.cef_client_t) *C.cef_keyboard_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.keyboardH
	}
	return nil
}

//export wailsCEFClientGetPermissionH
func wailsCEFClientGetPermissionH(self *C.cef_client_t) *C.cef_permission_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.permissionH
	}
	return nil
}

//export wailsCEFClientGetDragH
func wailsCEFClientGetDragH(self *C.cef_client_t) *C.cef_drag_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.dragH
	}
	return nil
}

//export wailsCEFClientGetRequestH
func wailsCEFClientGetRequestH(self *C.cef_client_t) *C.cef_request_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.requestH
	}
	return nil
}

//export wailsCEFKeyboardOnKeyEvent
func wailsCEFKeyboardOnKeyEvent(self *C.cef_keyboard_handler_t, browser *C.cef_browser_t, event *C.cef_key_event_t, osEvent C.cef_event_handle_t) C.int {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		return keyEvent(browser, event, bc)
	}
	return 0
}

//export wailsCEFPermissionOnMediaAccess
func wailsCEFPermissionOnMediaAccess(self *C.cef_permission_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, requestingOrigin *C.cef_string_t, requestedPermissions C.uint32_t, callback *C.cef_media_access_callback_t) C.int {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		return mediaPermission(browser, requestedPermissions, unsafe.Pointer(callback), bc)
	}
	return 0
}

//export wailsCEFDragOnEnter
func wailsCEFDragOnEnter(self *C.cef_drag_handler_t, browser *C.cef_browser_t, dragData *C.cef_drag_data_t, mask C.int) C.int {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		return dragEnter(browser, dragData, bc)
	}
	return 0
}

//export wailsCEFRequestOnBeforeBrowse
func wailsCEFRequestOnBeforeBrowse(self *C.cef_request_handler_t, browser *C.cef_browser_t, frame *C.cef_frame_t, request *C.cef_request_t, userGesture C.int, isRedirect C.int) C.int {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		return beforeBrowse(browser, request, bc)
	}
	return 0
}

//export wailsCEFDisplayHOnTitleChange
func wailsCEFDisplayHOnTitleChange(self *C.cef_display_handler_t, browser *C.cef_browser_t, title *C.cef_string_t) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		titleChange(browser, title, bc)
	}
}
