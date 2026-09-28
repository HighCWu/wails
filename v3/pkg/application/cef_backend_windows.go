//go:build windows && cgo && wails_cef && !server

package application

/*
#include <windows.h>
static void cef_pump_host(void) {
 MSG msg;
 for (int i=0;i<32 && PeekMessageW(&msg,NULL,0,0,PM_NOREMOVE);i++) {
  if(msg.message==WM_QUIT){PeekMessageW(&msg,NULL,0,0,PM_REMOVE);continue;}
  PeekMessageW(&msg,NULL,0,0,PM_REMOVE); TranslateMessage(&msg); DispatchMessageW(&msg);
 }
}
static void cef_size_child(uintptr_t parent,uintptr_t child) {
 RECT r; if(!child||!GetClientRect((HWND)parent,&r))return;
 RECT b; GetWindowRect((HWND)child,&b);
 if(b.right-b.left!=r.right || b.bottom-b.top!=r.bottom)
  SetWindowPos((HWND)child,NULL,0,0,r.right,r.bottom,SWP_NOACTIVATE|SWP_NOZORDER);
}
*/
import "C"

import (
	"fmt"
	"github.com/wailsapp/wails/v3/internal/assetserver"
	"github.com/wailsapp/wails/v3/internal/cef"
	wailsruntime "github.com/wailsapp/wails/v3/internal/runtime"
	"github.com/wailsapp/wails/v3/pkg/events"
	"github.com/wailsapp/wails/v3/pkg/w32"
)

func pumpCEFHost() { C.cef_pump_host() }
func init() {
	attachCEFWindows = attachWindowsCEF
	modalDepth := 0 // UI-thread only, including nested native dialogs.
	enterCEFModalLoop = func() func() {
		if !cef.Initialized() {
			return func() {}
		}
		modalDepth++
		cef.SetOSModalLoop(true)
		return func() { modalDepth--; cef.SetOSModalLoop(modalDepth != 0) }
	}
}
func attachWindowsCEF(w *windowsWebviewWindow) bool {
	if globalApplication.webviewBackend != WebviewBackendCEF {
		return false
	}
	if w.cefEngine != nil {
		return true
	}
	colour := w.parent.options.BackgroundColour
	r := w32.GetClientRect(w.hwnd)
	e := &desktopCEFEngine{id: w.parent.id, native: uintptr(w.hwnd), width: int(r.Right), height: int(r.Bottom), background: uint32(colour.Alpha)<<24 | uint32(colour.Red)<<16 | uint32(colour.Green)<<8 | uint32(colour.Blue)}
	e.closeNative = func() { w32.DestroyWindow(w.hwnd) }
	e.finishClose = func() {
		if w.parentHWND != 0 {
			w32.EnableWindow(w.parentHWND, true)
			w.parentHWND = 0
		}
		w.parent.markAsDestroyed()
		getNativeApplication().unregisterWindow(w)
	}
	e.syncNative = func() {
		if e.browser != nil {
			C.cef_size_child(C.uintptr_t(w.hwnd), C.uintptr_t(e.browser.XWindow()))
		}
	}
	e.loaded = func() {
		w.webviewNavigationCompleted = true
		js := wailsruntime.Core(globalApplication.impl.GetFlags(globalApplication.options))
		js += fmt.Sprintf("window._wails.flags.enableFileDrop=%v;window._wails.flags.frameless=%v;", w.parent.options.EnableFileDrop, w.parent.options.Frameless)
		js += w.parent.options.JS
		if w.parent.options.CSS != "" {
			js += fmt.Sprintf(";document.head.appendChild(document.createElement('style')).textContent=%q;", w.parent.options.CSS)
		}
		e.execJS(js)
		if !w.parent.options.Hidden {
			w.show()
		}
		w.parent.emit(events.Windows.WebViewNavigationCompleted)
	}
	desktopCEFEngines[e.id] = e
	w.cefEngine = e
	url, err := assetserver.GetStartURL(w.parent.options.URL)
	if err != nil {
		globalApplication.handleFatalError(err)
		return true
	}
	e.loadURL(url)
	return true
}
