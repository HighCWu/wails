//go:build linux && cgo && gtk3 && wails_cef && !android && !server

package application

/*
#cgo linux pkg-config: gtk+-3.0 webkit2gtk-4.1 gdk-3.0
#include <gtk/gtk.h>

// GTK signal trampolines for the CEF engine container widget (the engine
// file's preamble carries definitions, so exports live here).
extern void wailsCEFOnMap(GtkWidget* widget, gpointer user_data);
extern void wailsCEFOnSizeAllocate(GtkWidget* widget, GdkRectangle* allocation, gpointer user_data);
*/
import "C"

import (
	"fmt"
	"os"
	"unsafe"

	"github.com/wailsapp/wails/v3/internal/assetserver"
	"github.com/wailsapp/wails/v3/internal/cef"
	"github.com/wailsapp/wails/v3/pkg/events"
)

// This file wires the CEF backend into the GTK3 linux window
// implementation: process bootstrap, application lifecycle, glue hooks
// and the CEF flavour of the in-window webview engine.

func init() {
	// CEF re-executes this binary for renderer/gpu/utility subprocesses
	// with --type= switches. Those must run the CEF subprocess loop and
	// exit before any wails/GTK initialisation happens.
	if cef.IsSubprocess() {
		if err := cef.ExecuteSubprocess(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		// ExecuteSubprocess never returns.
	}

	// Expose CEF availability to the backend resolver.
	probeCEFRuntime = cef.Probe
}

//export wailsCEFOnMap
func wailsCEFOnMap(widget *C.GtkWidget, userData C.gpointer) {
	if e, ok := cefEngines.Load(unsafe.Pointer(widget)); ok {
		e.(*linuxCEFWebview).attach()
	}
}

//export wailsCEFOnSizeAllocate
func wailsCEFOnSizeAllocate(widget *C.GtkWidget, allocation *C.GdkRectangle, userData C.gpointer) {
	if e, ok := cefEngines.Load(unsafe.Pointer(widget)); ok {
		e.(*linuxCEFWebview).resizeBrowser(int(allocation.width), int(allocation.height))
	}
}

// initCEFBackend starts the CEF browser process when the resolved webview
// backend is CEF. Called from linuxApp.run() on the main thread, before
// the GTK main loop starts.
func initCEFBackend(app *App) error {
	if app.webviewBackend != WebviewBackendCEF {
		return nil
	}

	cef.SetStateHooks(&cef.State{
		DispatchMain: func(fn func()) { InvokeAsync(fn) },
		OnWindowMessage: func(windowID uint, message string, origin string) {
			windowMessageBuffer <- &windowMessage{
				windowId:   windowID,
				message:    message,
				originInfo: &OriginInfo{Origin: origin},
			}
		},
		OnWindowLoadEnd: func(windowID uint) {
			windowEvents <- &windowEvent{
				WindowID: windowID,
				EventID:  uint(events.Linux.WindowLoadFinished),
			}
		},
		OnWindowLoadStart: func(windowID uint) {
			windowEvents <- &windowEvent{
				WindowID: windowID,
				EventID:  uint(events.Linux.WindowLoadStarted),
			}
		},
		OnTitleChange: func(windowID uint, title string) {
			if window, ok := globalApplication.windows[windowID]; ok {
				impl := getLinuxWebviewWindow(window)
				if impl != nil {
					InvokeAsync(func() { impl.setTitle(title) })
				}
			}
		},
		OnBrowserClosed: func(windowID uint) {},
		AssetRequest: func(req *cef.AssetRequest) {
			windowName := ""
			if st := cef.Current(); st != nil && st.WindowName != nil {
				windowName = st.WindowName(req.WindowID)
			}
			webviewRequests <- &webViewAssetRequest{
				Request:    req.Request,
				windowId:   req.WindowID,
				windowName: windowName,
			}
		},
		WindowName: func(windowID uint) string {
			if window, ok := globalApplication.windows[windowID]; ok {
				return window.Name()
			}
			return ""
		},
	})

	// Serve the asset server from http://wails.localhost (the Windows
	// WebView2 pattern) — see internal/cef app.go for why not wails://.
	assetserver.SetBaseURL(cef.AssetScheme, cef.AssetHost)

	opts := cef.InitializeOptions{}
	if os.Getenv("WAILS_CEF_LOG_TO_FILE") == "1" {
		opts.LogToFile = true
	}
	return cef.Initialize(opts)
}

// shutdownCEFBackend tears CEF down after the GTK main loop has stopped.
// Called from appRun.
func shutdownCEFBackend() {
	cef.Shutdown()
}
