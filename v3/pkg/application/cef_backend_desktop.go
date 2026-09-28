//go:build cgo && wails_cef && (windows || (darwin && !ios)) && !server

package application

import (
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync/atomic"
	"time"

	"github.com/wailsapp/wails/v3/internal/assetserver"
	"github.com/wailsapp/wails/v3/internal/cef"
)

var desktopCEFEngines = map[uint]*desktopCEFEngine{}
var desktopCEFPumping atomic.Bool
var desktopCEFPumpActive bool   // UI-thread only: native loops can dispatch recursively.
var desktopCEFHostStopping bool // UI-thread only; shutdown still pumps browser closes.
var beginCEFHostWork = func() {}
var endCEFHostWork = func() {}

// CEF close callbacks run on the UI thread. Defer native destruction until the
// callback returns, and keep draining after Wails destroys its dispatch window.
var desktopCEFCloseTasks []func()

func drainDesktopCEFCloseTasks() {
	tasks := desktopCEFCloseTasks
	desktopCEFCloseTasks = nil
	for _, task := range tasks {
		task()
	}
}

func init() {
	runtime.LockOSThread()
	if cef.IsSubprocess() {
		if err := cef.ExecuteSubprocess(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	probeCEFRuntime = cef.Probe
	startPlatformCEF = startDesktopCEF
	stopPlatformCEF = func() {
		if desktopCEFPumping.Swap(false) {
			cef.Shutdown()
		}
	}
}

func startDesktopCEF(app *App) error {
	if app.webviewBackend != WebviewBackendCEF {
		return nil
	}
	cef.SetStateHooks(&cef.State{
		DispatchMain: func(fn func()) { InvokeAsync(fn) },
		PumpHostLoop: func() { pumpCEFHost(); drainDesktopCEFCloseTasks() },
		OnWindowMessage: func(id uint, message, origin string) {
			windowMessageBuffer <- &windowMessage{windowId: id, message: message, originInfo: &OriginInfo{Origin: origin}}
		},
		AssetRequest: func(req *cef.AssetRequest) {
			name := ""
			if w, ok := app.Window.GetByID(req.WindowID); ok {
				name = w.Name()
			}
			webviewRequests <- &webViewAssetRequest{Request: cefAssetRequest{req.Request}, windowId: req.WindowID, windowName: name}
		},
		OnWindowLoadEnd: func(id uint) {
			if e := desktopCEFEngines[id]; e != nil && e.loaded != nil {
				e.loaded()
			}
		},
		OnTitleChange: func(id uint, title string) {
			if w, ok := app.Window.GetByID(id); ok {
				w.SetTitle(title)
			}
		},
		OnBrowserClosing: func(id uint) bool {
			if e := desktopCEFEngines[id]; e != nil {
				e.closing = true
				desktopCEFCloseTasks = append(desktopCEFCloseTasks, e.closeNative)
				return true
			}
			return false
		},
		OnBrowserClosed: func(id uint) {
			desktopCEFCloseTasks = append(desktopCEFCloseTasks, func() {
				if e := desktopCEFEngines[id]; e != nil {
					delete(desktopCEFEngines, id)
					e.finishClose()
				}
			})
		},
		OnKeyEvent: func(id uint, key, modifiers uint32) bool {
			var acc accelerator
			if modifiers&cef.EventFlagControlDown != 0 {
				acc.Modifiers = append(acc.Modifiers, ControlKey)
			}
			if modifiers&cef.EventFlagShiftDown != 0 {
				acc.Modifiers = append(acc.Modifiers, ShiftKey)
			}
			if modifiers&cef.EventFlagAltDown != 0 {
				acc.Modifiers = append(acc.Modifiers, OptionOrAltKey)
			}
			if modifiers&(1<<7) != 0 {
				acc.Modifiers = append(acc.Modifiers, SuperKey)
			}
			var ok bool
			acc.Key, ok = cefNativeKey(key)
			if !ok {
				return false
			}
			if w, ok := app.Window.GetByID(id); ok {
				if window, ok := w.(*WebviewWindow); ok {
					return window.processKeyBinding(acc.String())
				}
			}
			return false
		},
		OnFileDrop: func(id uint, files []string, x, y int) {
			InvokeAsync(func() {
				w, ok := app.Window.GetByID(id)
				if !ok {
					return
				}
				window, ok := w.(*WebviewWindow)
				if ok && window.options.EnableFileDrop {
					window.InitiateFrontendDropProcessing(files, x, y)
				}
			})
		},
		OnMediaPermission: func(id uint, audio, video bool) bool {
			w, ok := app.Window.GetByID(id)
			if !ok {
				return false
			}
			window, ok := w.(*WebviewWindow)
			if !ok {
				return false
			}
			permissions := window.options.Permissions
			return (!audio || permissions[PermissionMicrophone] == PermissionAllow) && (!video || permissions[PermissionCamera] == PermissionAllow)
		},
		OnRenderCrash: func(id uint, status int) {
			if w, ok := app.Window.GetByID(id); ok {
				w.EmitEvent(events.Common.WindowRenderCrash)
			}
			app.warning("CEF renderer process crashed (status %d)", status)
		},
	})
	assetserver.SetBaseURL(cef.AssetScheme, cef.AssetHost)
	if err := cef.Initialize(cef.InitializeOptions{LogToFile: os.Getenv("WAILS_CEF_LOG_TO_FILE") == "1"}); err != nil {
		return err
	}
	desktopCEFPumping.Store(true)
	go func() {
		ticker := time.NewTicker(10 * time.Millisecond)
		defer ticker.Stop()
		for range ticker.C {
			if !desktopCEFPumping.Load() {
				return
			}
			InvokeAsync(func() {
				// Cocoa's nested NSApplication run must unwind before another
				// pump or termination. Win32's OS-modal loop still needs pumping.
				if !desktopCEFPumping.Load() || desktopCEFHostStopping || (runtime.GOOS == "darwin" && desktopCEFPumpActive) {
					return
				}
				desktopCEFPumpActive = true
				beginCEFHostWork()
				defer func() {
					desktopCEFPumpActive = false
					endCEFHostWork()
				}()
				cef.DoMessageLoopWork()
				drainDesktopCEFCloseTasks()
				for _, e := range desktopCEFEngines {
					if !e.closing && e.syncNative != nil {
						e.syncNative()
					}
				}
			})
		}
	}()
	return nil
}

type desktopCEFEngine struct {
	browser       *cef.Browser
	id            uint
	native        uintptr
	width, height int
	background    uint32
	closing       bool
	closeNative   func()
	finishClose   func()
	loaded        func()
	syncNative    func()
}

func (e *desktopCEFEngine) loadURL(url string) {
	if e.browser != nil {
		e.browser.LoadURL(url)
		return
	}
	b, err := cef.CreateBrowser(cef.CreateBrowserOptions{WindowID: e.id, ParentXWindow: e.native, Width: e.width, Height: e.height, URL: url, BackgroundColor: e.background})
	if err != nil {
		globalApplication.handleFatalError(err)
		return
	}
	e.browser = b
}
func (e *desktopCEFEngine) execJS(js string) {
	InvokeAsync(func() {
		if e.browser != nil && !e.closing {
			e.browser.ExecJS(js)
		}
	})
}
func (e *desktopCEFEngine) reload(ignore bool) {
	if e.browser != nil {
		e.browser.Reload(ignore)
	}
}
func (e *desktopCEFEngine) setZoomFactor(zoom float64) {
	if e.browser != nil {
		e.browser.SetZoomFactor(zoom)
	}
}
func (e *desktopCEFEngine) zoomFactor() float64 {
	if e.browser != nil {
		return e.browser.ZoomFactor()
	}
	return 1
}
func (e *desktopCEFEngine) focus() {
	if e.browser != nil {
		e.browser.Focus()
	}
}
func (e *desktopCEFEngine) openDevTools() {
	if e.browser != nil {
		e.browser.OpenDevTools()
	}
}
func (e *desktopCEFEngine) close() {
	if e.closing {
		return
	}
	e.closing = true
	if e.browser != nil {
		e.browser.Close(true)
	} else {
		delete(desktopCEFEngines, e.id)
		e.closeNative()
		e.finishClose()
	}
}

// CEF supplies Windows virtual-key codes on both Win32 and Cocoa.
func cefNativeKey(key uint32) (string, bool) {
	if key >= 0x41 && key <= 0x5a || key >= 0x30 && key <= 0x39 {
		return strings.ToLower(string(rune(key))), true
	}
	if key >= 0x70 && key <= 0x87 {
		return fmt.Sprintf("f%d", key-0x6f), true
	}
	value, ok := map[uint32]string{8: "backspace", 9: "tab", 13: "return", 27: "escape", 32: "space", 33: "page up", 34: "page down", 35: "end", 36: "home", 37: "left", 38: "up", 39: "right", 40: "down", 45: "insert", 46: "delete", 186: ";", 187: "=", 188: ",", 189: "-", 190: ".", 191: "/", 192: "`", 219: "[", 220: "\\", 221: "]", 222: "'"}[key]
	return value, ok
}
