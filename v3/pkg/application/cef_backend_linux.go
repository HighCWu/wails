//go:build linux && cgo && wails_cef && !android && !server

package application

/*
#cgo gtk3 pkg-config: gtk+-3.0 gdk-3.0
#cgo !gtk3 pkg-config: gtk4
#include <gtk/gtk.h>
unsigned int wails_cef_keyval(unsigned int keycode);

// The pump callback: calls into the cef package's DoMessageLoopWork.
extern gboolean wailsCEFPumpMessageLoop(gpointer user_data);
static guint wails_cef_pump_source;
static void wails_cef_start_message_pump(void) {
  wails_cef_pump_source = g_timeout_add(10, wailsCEFPumpMessageLoop, NULL);
}

// GTK signal trampolines for the CEF engine container widget (the engine
// file's preamble carries definitions, so exports live here).
extern void wailsCEFOnMap(GtkWidget* widget, gpointer user_data);
extern void wailsCEFOnSizeAllocate(GtkWidget* widget, GdkRectangle* allocation, gpointer user_data);
static void wails_cef_stop_message_pump(void) {
 if (wails_cef_pump_source) { g_source_remove(wails_cef_pump_source); wails_cef_pump_source = 0; }
}
static void wails_cef_drain_host(void) {
 for (int i = 0; i < 32 && g_main_context_pending(NULL); i++) g_main_context_iteration(NULL, FALSE);
}
*/
import "C"

import (
	"fmt"
	"os"
	"strings"
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
	// The widget is realized now (its X window exists); this is the
	// reliable attach driver once startURL has arrived via setURL
	// (attach is a no-op until then). Earlier size-allocate attempts
	// may have run before realize and failed to obtain the XID.
	if e, ok := cefEngines.Load(unsafe.Pointer(widget)); ok {
		e.(*linuxCEFWebview).attach()
	}
}

//export wailsCEFPumpMessageLoop
func wailsCEFPumpMessageLoop(userData C.gpointer) C.gboolean {
	cef.DoMessageLoopWork()
	// Lazy attach driver (see tryAttach): runs on the GTK main thread.
	cefEngines.Range(func(_, v any) bool {
		engine := v.(*linuxCEFWebview)
		engine.tryAttach()
		engine.syncSize()
		return true
	})
	return C.gboolean(1) // keep the source
}

//export wailsCEFOnFocusIn
func wailsCEFOnFocusIn(widget *C.GtkWidget, event *C.GdkEvent, userData C.gpointer) C.gboolean {
	if e := engineForToplevel(unsafe.Pointer(widget)); e != nil {
		e.focusBrowser()
		// Match cefclient: GTK must not take focus back from the native browser.
		return C.gboolean(1)
	}
	return C.gboolean(0)
}

//export wailsCEFOnSizeAllocate
func wailsCEFOnSizeAllocate(widget *C.GtkWidget, allocation *C.GdkRectangle, userData C.gpointer) {
	if e, ok := cefEngines.Load(unsafe.Pointer(widget)); ok {
		engine := e.(*linuxCEFWebview)
		// First valid allocation drives browser creation — the drawing
		// area's absolute origin is only reliable after toplevel layout.
		if engine.browserOrWait() == nil {
			engine.attach()
			return
		}
		engine.resizeBrowser(int(allocation.width), int(allocation.height))
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
		PumpHostLoop: func() { C.wails_cef_drain_host() },
		OnWindowMessage: func(windowID uint, message string, origin string) {
			if message == "wails:drag" || strings.HasPrefix(message, "wails:resize:") {
				edge := strings.TrimPrefix(message, "wails:resize:")
				if message == "wails:drag" {
					edge = ""
				} else if !validCEFResizeEdge(edge) {
					return
				}
				InvokeAsync(func() {
					if window, ok := globalApplication.Window.GetByID(windowID); ok {
						if e := getLinuxEngine(window); e != nil && !window.IsFullscreen() {
							if edge == "" || !e.parent.parent.options.DisableResize {
								e.beginWindowDrag(edge)
							}
						}
					}
				})
				return
			}
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
		OnBrowserClosed: func(windowID uint) { InvokeAsync(func() { finishCEFWindowClose(windowID) }) },
		AssetRequest: func(req *cef.AssetRequest) {
			windowName := ""
			if st := cef.Current(); st != nil && st.WindowName != nil {
				windowName = st.WindowName(req.WindowID)
			}
			webviewRequests <- &webViewAssetRequest{
				Request:    cefAssetRequest{req.Request},
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
		OnKeyEvent: func(windowID uint, nativeKeyCode uint32, modifiers uint32) bool {
			accelerator, ok := cefAccelerator(nativeKeyCode, modifiers)
			if !ok {
				return false
			}
			windowKeyEvents <- &windowKeyEvent{
				windowId:          windowID,
				acceleratorString: accelerator,
			}
			// Consume undo/redo for the same reason the WebKit path
			// does: the native handler is unreliable for inputs and
			// handleKeyEvent re-issues it via execCommand.
			return accelerator == "Ctrl+Z" || accelerator == "Ctrl+Shift+Z"
		},
		OnMediaPermission: func(windowID uint, needAudio, needVideo bool) bool {
			return allowMediaCapture(windowID, needAudio, needVideo)
		},
		OnRenderCrash: func(windowID uint, status int) {
			// Mirror the desktop glue: surface the renderer death as the
			// typed window event so OnWindowEvent listeners see it.
			if window, ok := globalApplication.Window.GetByID(windowID); ok {
				if ww, ok := window.(*WebviewWindow); ok {
					ww.emit(events.Common.WindowRenderCrash)
				}
			}
			globalApplication.warning("CEF renderer process crashed (status %d)", status)
		},
	})

	// Serve the asset server from http://wails.localhost (the Windows
	// WebView2 pattern) — see internal/cef app.go for why not wails://.
	assetserver.SetBaseURL(cef.AssetScheme, cef.AssetHost)

	opts := cef.InitializeOptions{}
	if os.Getenv("WAILS_CEF_LOG_TO_FILE") == "1" {
		opts.LogToFile = true
	}
	if err := cef.Initialize(opts); err != nil {
		return err
	}

	// MTML is off: pump CEF's message loop from the GTK main loop. The
	// source is attached before g_application_run starts; 10ms keeps CEF
	// responsive (~60-100fps budget) without busy-spinning.
	C.wails_cef_start_message_pump()
	return nil
}

// shutdownCEFBackend tears CEF down after the GTK main loop has stopped.
// Called from appRun.
func shutdownCEFBackend() {
	if !cef.Initialized() {
		return
	}
	C.wails_cef_stop_message_pump()
	// GTK may quit before flushing the final XDestroyWindow. CEF uses a
	// separate X connection and must see native child destruction first.
	C.gdk_display_sync(C.gdk_display_get_default())
	cef.Shutdown()
}

// cefAccelerator converts a CEF key event (X keycode + EVENTFLAG modifier
// mask) into the wails accelerator string used by key bindings and menus.
func cefAccelerator(nativeKeyCode, modifiers uint32) (string, bool) {
	var acc accelerator
	if modifiers&cef.EventFlagShiftDown != 0 {
		acc.Modifiers = append(acc.Modifiers, ShiftKey)
	}
	if modifiers&cef.EventFlagControlDown != 0 {
		acc.Modifiers = append(acc.Modifiers, ControlKey)
	}
	if modifiers&cef.EventFlagAltDown != 0 {
		acc.Modifiers = append(acc.Modifiers, OptionOrAltKey)
	}
	keyString, ok := VirtualKeyCodes[uint(C.wails_cef_keyval(C.uint(nativeKeyCode)))]
	if !ok {
		return "", false
	}
	acc.Key = keyString
	return acc.String(), true
}

func validCEFResizeEdge(edge string) bool {
	switch edge {
	case "n-resize", "ne-resize", "e-resize", "se-resize", "s-resize", "sw-resize", "w-resize", "nw-resize":
		return true
	}
	return false
}
