//go:build darwin && cgo && wails_cef && !ios && !server

package application

/*
#cgo CFLAGS: -DWAILS_CEF
#include <stdint.h>
void wails_cef_prepare_app(void);
void wails_cef_pump_host(void);
uintptr_t wails_cef_content_view(void* window);
void wails_cef_size_view(uintptr_t parent,uintptr_t child);
void wails_cef_close_window(void* window);
*/
import "C"

import "github.com/wailsapp/wails/v3/pkg/events"

func pumpCEFHost() { C.wails_cef_pump_host() }
func init() {
	preparePlatformCEF = func(app *App) error {
		if app.webviewBackend == WebviewBackendCEF {
			C.wails_cef_prepare_app()
		}
		return nil
	}
	attachCEFDarwin = func(w *macosWebviewWindow) {
		if globalApplication.webviewBackend != WebviewBackendCEF {
			return
		}
		colour := w.parent.options.BackgroundColour
		e := &desktopCEFEngine{id: w.parent.id, native: uintptr(C.wails_cef_content_view(w.nsWindow)), width: w.parent.options.Width, height: w.parent.options.Height, background: uint32(colour.Alpha)<<24 | uint32(colour.Red)<<16 | uint32(colour.Green)<<8 | uint32(colour.Blue)}
		e.syncNative = func() {
			if e.browser != nil {
				C.wails_cef_size_view(C.uintptr_t(e.native), C.uintptr_t(e.browser.XWindow()))
			}
		}
		e.loaded = func() { w.parent.emit(events.Mac.WebViewDidFinishNavigation) }
		e.finishClose = func() {
			w.parent.markAsDestroyed()
			clearWindowDragCache(w.parent.id)
			C.wails_cef_close_window(w.nsWindow)
			w.nsWindow = nil
		}
		desktopCEFEngines[e.id] = e
		w.cefEngine = e
	}
}

//export wailsCEFEnabled
func wailsCEFEnabled() C.int {
	if globalApplication != nil && globalApplication.webviewBackend == WebviewBackendCEF {
		return 1
	}
	return 0
}
