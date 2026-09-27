//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}
*/
import "C"

import "unsafe"

// Exported C callbacks (invoked by CEF through the static shims declared
// in each area file's preamble). Every function must keep its preamble
// declaration-only; the Go bodies live here and delegate to unexported
// handlers next to the code that owns the struct.

// --- cef_app_t ---

//export wailsCEFAppRegisterSchemes
func wailsCEFAppRegisterSchemes(self *C.cef_app_t, registrar *C.cef_scheme_registrar_t) {
	appRegisterSchemes(registrar)
}

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

//export wailsCEFRPHWebKitInitialized
func wailsCEFRPHWebKitInitialized(self *C.cef_render_process_handler_t) {
	renderWebKitInitialized()
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

//export wailsCEFDisplayHOnTitleChange
func wailsCEFDisplayHOnTitleChange(self *C.cef_display_handler_t, browser *C.cef_browser_t, title *C.cef_string_t) {
	if bc := clientByHandlerPtr(unsafe.Pointer(self)); bc != nil {
		titleChange(browser, title, bc)
	}
}
