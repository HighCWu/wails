package main

import (
	"embed"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"time"

	"github.com/wailsapp/wails/v3/pkg/application"
	"github.com/wailsapp/wails/v3/pkg/events"
)

//go:embed assets
var assets embed.FS

// CompatService exposes an echo binding used by the frontend to measure the
// frontend->Go call path on whichever webview backend is running.
type CompatService struct{}

func (s *CompatService) Ping(payload string) string { return payload }

// ImeReport receives IME composition milestones from the IME leg
// (ci/test-ime.sh): the host log line is the assertion surface.
func (s *CompatService) ImeReport(event string, data string) string {
	fmt.Println("compat: IME " + event + " " + data)
	return "ok"
}

// Fail always errors: the frontend asserts the error propagates as a
// rejected Promise (bindings error path).
func (s *CompatService) Fail(msg string) (string, error) {
	return "", errors.New("intentional: " + msg)
}

var failures int

func check(win *application.WebviewWindow, app *application.App, name string, ok bool, detail string) {
	status := "PASS"
	if !ok {
		status = "FAIL"
		failures++
	}
	app.Logger.Info(fmt.Sprintf("compat: %s %s %s", name, status, detail))
}

func main() {
	var app *application.App
	opts := application.Options{
		Name: "webview-compat",
		Assets: application.AssetOptions{
			Handler: application.BundledAssetFileServer(assets),
		},
		Services: []application.Service{
			application.NewService(&CompatService{}),
		},
	}
	if os.Getenv("WAILS_COMPAT_SINGLEINSTANCE") == "1" {
		// second-instance lock: the driver launches this binary twice and
		// asserts the first instance receives the launch callback
		opts.SingleInstance = &application.SingleInstanceOptions{
			UniqueID: "com.wails.compat.singleinstance",
			OnSecondInstanceLaunch: func(data application.SecondInstanceData) {
				// app is assigned by the time a second instance can launch
				app.Logger.Info(fmt.Sprintf(
					"compat: SECOND-INSTANCE args=%v workdir=%s", data.Args, data.WorkingDir))
			},
		}
	}
	app = application.New(opts)

	win := app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name:      "main",
		Title:     "webview-compat",
		Width:     800,
		Height:    600,
		Frameless: os.Getenv("WAILS_COMPAT_FRAMELESS") == "1",
	})

	// Window-event parity: the impl must raise the Common events (state
	// transitions and debounced resize/move) — asserted at suite end.
	var evFocus, evResize, evMaximise, evShow int32
	// reload leg: count bench completions; the second one proves the page
	// came back and re-ran the full transport matrix after the reload
	var benchFinished int32
	win.OnWindowEvent(events.Common.WindowFocus, func(*application.WindowEvent) {
		atomic.AddInt32(&evFocus, 1)
	})
	win.OnWindowEvent(events.Common.WindowDidResize, func(*application.WindowEvent) {
		atomic.AddInt32(&evResize, 1)
	})
	win.OnWindowEvent(events.Common.WindowMaximise, func(*application.WindowEvent) {
		atomic.AddInt32(&evMaximise, 1)
	})
	win.OnWindowEvent(events.Common.WindowShow, func(*application.WindowEvent) {
		atomic.AddInt32(&evShow, 1)
	})

	// InvokeSync liveness probe on the electron backend (audit follow-up)
	go func() {
		time.Sleep(5 * time.Second)
		done := make(chan struct{})
		go func() {
			application.InvokeSync(func() {
				app.Logger.Info("compat: INVOKE-SYNC WORKS")
				close(done)
			})
		}()
		select {
		case <-done:
		case <-time.After(8 * time.Second):
			app.Logger.Info("compat: INVOKE-SYNC HUNG")
		}
	}()

	// Crash recovery: the backend recovers the page with one auto-reload
	// and reports through the custom event (no typed upstream event
	// exists for renderer crashes).
	app.Event.On("electron:rendererCrashed", func(e *application.CustomEvent) {
		app.Logger.Info("compat: CRASH-EVENT", "data", e.Data)
	})

	// bindings error path: the frontend calls CompatService.Fail and the
	// rejection must carry the Go error text back
	var rpcErrProp int32
	app.Event.On("compat:rpc-error-prop", func(e *application.CustomEvent) {
		if msg, ok := e.Data.(string); ok && strings.Contains(msg, "intentional") {
			atomic.StoreInt32(&rpcErrProp, 1)
		}
	})

	// The frontend measures the frontend->Go call path (bindings) and
	// reports a latency table; CI compares it across backends.
	app.Event.On("compat:rpc", func(e *application.CustomEvent) {
		if data, ok := e.Data.(map[string]any); ok {
			app.Logger.Info(fmt.Sprintf(
				"compat: RPC n=%v avg=%vms p50=%vms p95=%vms",
				data["n"], data["avg"], data["p50"], data["p95"]))
		}
	})
	app.Event.On("compat:rpc-error", func(e *application.CustomEvent) {
		if msg, ok := e.Data.(string); ok {
			app.Logger.Error("compat: rpc-error " + msg)
		}
	})
	app.Event.On("compat:bench", func(e *application.CustomEvent) {
		if data, ok := e.Data.(map[string]any); ok {
			app.Logger.Info(fmt.Sprintf(
				"compat: BENCH path=%v size=%v n=%v avg=%vms p50=%vms p95=%vms p99=%vms",
				data["path"], data["size"], data["n"], data["avg"], data["p50"], data["p95"], data["p99"]))
		}
	})
	app.Event.On("compat:bench-error", func(e *application.CustomEvent) {
		if msg, ok := e.Data.(string); ok {
			app.Logger.Error("compat: BENCH-ERROR " + msg)
		}
	})
	app.Event.On("compat:bench-done", func(e *application.CustomEvent) {
		if name, ok := e.Data.(string); ok {
			app.Logger.Info(fmt.Sprintf("compat: BENCH-DONE path=%s", name))
		}
	})
	app.Event.On("compat:bench-finished", func(e *application.CustomEvent) {
		atomic.AddInt32(&benchFinished, 1)
		app.Logger.Info("compat: BENCH-FINISHED")
	})

	app.Event.OnApplicationEvent(events.Common.ApplicationStarted, func(*application.ApplicationEvent) {
		go func() {
			app.Logger.Info(fmt.Sprintf("compat: backend=%s", app.WebviewBackend()))
			time.Sleep(1 * time.Second) // let the page finish loading
			if os.Getenv("WAILS_COMPAT_DEVTOOLS") == "1" {
				// open DevTools (detach mode) before the suite, verify the
				// main window still works with it open, and let the app
				// exit with DevTools up — the shutdown path must account
				// for the DevTools webContents
				win.OpenDevTools()
				time.Sleep(2 * time.Second)
				app.Logger.Info("compat: DEVTOOLS opened")
			}
			if os.Getenv("WAILS_COMPAT_CHURN") == "1" {
				// independent windows; runs regardless of the suite
				// verdict so known suite gaps can't gate lifecycle work
				runChurn(app)
			}
			if os.Getenv("WAILS_COMPAT_DIALOGS") == "1" {
				// interactive dialog scenario: the driver script watches
				// for DIALOG-OPENED / MSG-OPENED and drives the real
				// dialogs with xdotool (Escape cancels the chooser,
				// Return confirms the message box). Gated on a go-file so
				// the dialogs run AFTER the suite (its window ops would
				// otherwise dismiss them mid-flight).
				go func() {
					for {
						if _, err := os.Stat(filepath.Join(os.TempDir(), "compat-dialogs-go")); err == nil {
							break
						}
						time.Sleep(300 * time.Millisecond)
					}
					app.Logger.Info("compat: DIALOG-OPENED")
					path, err := app.Dialog.OpenFile().PromptForSingleSelection()
					if err != nil {
						app.Logger.Info("compat: DIALOG-RESULT err=" + err.Error())
					} else if path == "" {
						app.Logger.Info("compat: DIALOG-RESULT canceled")
					} else {
						app.Logger.Info("compat: DIALOG-RESULT path=" + path)
					}
					app.Logger.Info("compat: MSG-OPENED")
					q := app.Dialog.Question().SetMessage("compat dialogs?")
					q.AttachToWindow(win)
					q.AddButton("OK")
					q.Show()
					time.Sleep(2 * time.Second)
					app.Logger.Info("compat: DIALOGS-DONE")
				}()
			}
			if os.Getenv("WAILS_COMPAT_MENUS") == "1" {
				// interactive context-menu scenario: the driver presses
				// Down + Return on the real popup; the item callback
				// reports the selection through the wails state machine.
				// Gated like the dialogs — the suite's window ops would
				// dismiss the popup.
				cm := application.NewMenu()
				cm.Add("compat-menu-item").OnClick(func(*application.Context) {
					app.Logger.Info("compat: MENU-CLICKED")
				})
				app.ContextMenu.Add("compat-menu", &application.ContextMenu{Menu: cm})
				go func() {
					// serialize after the dialog scenario: a concurrent
					// message dialog (modal) would dismiss the popup
					for {
						if _, err := os.Stat(filepath.Join(os.TempDir(), "compat-dialogs-done")); err == nil {
							break
						}
						time.Sleep(300 * time.Millisecond)
					}
					app.Logger.Info("compat: MENU-OPENED")
					win.OpenContextMenu(&application.ContextMenuData{
						Id: "compat-menu", X: 40, Y: 60,
					})
				}()
			}
			runSuite(app, win)
			check(win, app, "rpc-error-prop",
				atomic.LoadInt32(&rpcErrProp) == 1,
				fmt.Sprintf("propagated=%d", atomic.LoadInt32(&rpcErrProp)))
			// Window-event parity is asserted on the electron backend only:
			// the GTK impl does not surface focus/maximise state signals as
			// Common events (upstream gap), so the full set is its own task.
			if os.Getenv("WAILS_WEBVIEW_BACKEND") == "electron" {
				check(win, app, "events",
					atomic.LoadInt32(&evFocus) > 0 && atomic.LoadInt32(&evResize) > 0 &&
						atomic.LoadInt32(&evMaximise) > 0 && atomic.LoadInt32(&evShow) > 0,
					fmt.Sprintf("focus=%d resize=%d maximise=%d show=%d",
						atomic.LoadInt32(&evFocus), atomic.LoadInt32(&evResize),
						atomic.LoadInt32(&evMaximise), atomic.LoadInt32(&evShow)))
			}
			if failures > 0 {
				app.Logger.Info(fmt.Sprintf("compat: SUITE FAIL failures=%d", failures))
				return
			}
			app.Logger.Info("compat: SUITE PASS")
			if os.Getenv("WAILS_COMPAT_APIEXT") == "1" {
				// electron-backend API extensions: accelerator feed
				// (before-input-event), menubar + item accelerators,
				// and the intercepted X-close → WindowClosing chain.
				// The driver presses the combos with xdotool and ends
				// with Alt+F4; CLOSING-EVENT must appear before exit.
				win.RegisterKeyBinding("Ctrl+Shift+K", func(application.Window) {
					app.Logger.Info("compat: ACCEL-FIRED ctrl+shift+k")
				})
				mb := application.NewMenu()
				mb.Add("CompatAction").SetAccelerator("Ctrl+Shift+M").OnClick(func(*application.Context) {
					app.Logger.Info("compat: MENUBAR-CLICKED")
				})
				check := mb.AddCheckbox("CompatCheck", true)
				win.SetMenu(mb)
				// programmatic state change must re-serialize + re-push the
				// menubar (the driver asserts a second menu-set event)
				go func() {
					time.Sleep(3 * time.Second)
					check.SetChecked(false)
					app.Logger.Info("compat: CHECK-SET false")
				}()
				win.OnWindowEvent(events.Common.WindowClosing, func(*application.WindowEvent) {
					app.Logger.Info("compat: CLOSING-EVENT")
				})
				win.Flash(true)
				time.Sleep(200 * time.Millisecond)
				win.Flash(false)
				// global shortcut: OS-level grab (X11 / RegisterHotKey),
				// backend-independent — proves the platform layer under
				// the electron process
				if err := app.GlobalShortcut.Register("Ctrl+Shift+G", func() {
					app.Logger.Info("compat: GLOBAL-SHORTCUT-FIRED ctrl+shift+g")
				}); err != nil {
					app.Logger.Info("compat: GLOBAL-SHORTCUT-ERR " + err.Error())
				}
				// JS-initiated close on a secondary window: window.close()
				// fires electron 'close', the addon preventDefaults it, Go
				// runs the WindowClosing chain and the default listener
				// destroys — asserting the chain without keyboard input
				win2 := app.Window.NewWithOptions(application.WebviewWindowOptions{
					Name: "apiext-second", Title: "apiext-second",
					Width: 300, Height: 200,
				})
				win2.OnWindowEvent(events.Common.WindowClosing, func(*application.WindowEvent) {
					app.Logger.Info("compat: WIN2-CLOSING")
				})
				go func() {
					time.Sleep(2 * time.Second)
					win2.ExecJS("window.close()")
					deadline := time.Now().Add(10 * time.Second)
					for {
						if _, ok := app.Window.GetByID(win2.ID()); !ok {
							app.Logger.Info("compat: WIN2-GONE")
							return
						}
						if time.Now().After(deadline) {
							app.Logger.Info("compat: WIN2-STUCK")
							return
						}
						time.Sleep(200 * time.Millisecond)
					}
				}()
				app.Logger.Info("compat: APIEXT-ARMED")
			}
			if os.Getenv("WAILS_COMPAT_TRAY") == "1" {
				// electron-backend tray mapping: create + setters + menu
				// uid round-trip. Visual presence and tray-icon clicks
				// need a tray host (none exists on a bare Xvfb); the
				// driver asserts the lifecycle markers and the program
				//matic openMenu menu-click chain.
				icon := make([]byte, 4*16*16) // dummy RGBA payload
				for i := range icon {
					icon[i] = 0x80
				}
				tray := app.SystemTray.New()
				tray.SetIcon(icon)
				tray.SetTooltip("compat tray")
				mt := application.NewMenu()
				mt.Add("TrayItem").OnClick(func(*application.Context) {
					app.Logger.Info("compat: TRAY-CLICKED")
				})
				tray.SetMenu(mt)
				tray.OnClick(func() {
					app.Logger.Info("compat: TRAY-LEFT-CLICK")
				})
				// attached window: ToggleWindow is the default click
				// handler — driving it programmatically exercises the
				// bounds/positionWindow/getScreen chain under electron
				tw := app.Window.NewWithOptions(application.WebviewWindowOptions{
					Name: "tray-attached", Title: "tray-attached",
					Width: 300, Height: 200, Hidden: true,
				})
				tray.AttachWindow(tw).WindowOffset(5)
				tray.Run()
				app.Logger.Info(fmt.Sprintf(
					"compat: TRAY-ARMED attached-visible=%v main-visible=%v",
					tw.IsVisible(), win.IsVisible()))
				app.Logger.Info("compat: TRAY-ARMED")
				go func() {
					time.Sleep(2 * time.Second)
					tray.ToggleWindow()
					// visibility state rides the async event stream —
					// give the show/hide events time to land
					time.Sleep(1 * time.Second)
					app.Logger.Info(fmt.Sprintf(
						"compat: TRAY-TOGGLE visible=%v", tw.IsVisible()))
					tray.ToggleWindow()
					time.Sleep(1 * time.Second)
					app.Logger.Info(fmt.Sprintf(
						"compat: TRAY-TOGGLE2 visible=%v", tw.IsVisible()))
					// programmatic menu open → click round trip
					tray.OpenMenu()
					app.Logger.Info("compat: TRAY-OPEN-CALLED")
				}()
			}
			if os.Getenv("WAILS_COMPAT_RELOAD") == "1" {
				// Same-process reload: the renderer re-injects into the
				// reloaded page and must re-dial the bridge endpoint. The
				// re-run transport bench over the NEW connection is the
				// end-to-end proof; a leaked old fd shows up as a stranded
				// serve goroutine, a broken re-injection as a timeout.
				// the suite and the bench run concurrently: wait for
				// bench #1 to complete first, or the reload aborts it
				// mid-run and the completion counter never reaches two
				deadline := time.Now().Add(180 * time.Second)
				for atomic.LoadInt32(&benchFinished) < 1 && time.Now().Before(deadline) {
					time.Sleep(300 * time.Millisecond)
				}
				app.Logger.Info("compat: RELOAD-BEGIN")
				win.ExecJS("window.location.reload()")
				deadline = time.Now().Add(180 * time.Second)
				for atomic.LoadInt32(&benchFinished) < 2 && time.Now().Before(deadline) {
					time.Sleep(300 * time.Millisecond)
				}
				if atomic.LoadInt32(&benchFinished) >= 2 {
					app.Logger.Info("compat: RELOAD-OK")
				} else {
					app.Logger.Info("compat: RELOAD-TIMEOUT")
				}
			}
		}()
	})

	err := app.Run()
	if err != nil {
		log.Fatal(err)
	}
}

// runChurn exercises window create/close churning: even windows are
// closed after their page had time to load, odd windows are closed while
// creation is still in flight. Every window must end up removed from the
// window manager — a leak here hides double-create or missed-destroy
// bugs behind a growing window count.
//
// KNOWN LIMITATION (electron backend, Electron 44 / X11): the first close
// followed by another window creation wedges the Electron main loop —
// no control-protocol request is dispatched again (investigated across
// graceful close, destroy, deferred destroy, fs.read control channel;
// cf. electron/electron#29050 for the domain). The churn leg is therefore
// NOT enabled in CI for the electron backend until the runtime is
// upgraded or the failure mode is pinned down further.
func runChurn(app *application.App) {
	for i := 0; i < 5; i++ {
		even := i%2 == 0
		win := app.Window.NewWithOptions(application.WebviewWindowOptions{
			Name:   fmt.Sprintf("churn-%d", i),
			Title:  fmt.Sprintf("churn-%d", i),
			URL:    "/blank.html", // no runtime, no bench side effects
			Width:  320,
			Height: 200,
		})
		id := win.ID()
		if even {
			time.Sleep(1200 * time.Millisecond) // let the page settle
		}
		win.Close()
		time.Sleep(300 * time.Millisecond) // let the backend drain the destroy
		deadline := time.Now().Add(5 * time.Second)
		for {
			if _, ok := app.Window.GetByID(id); !ok {
				app.Logger.Info(fmt.Sprintf(
					"compat: CHURN window=%d id=%d mode=%s removed=yes",
					i, id, map[bool]string{true: "settled", false: "pending"}[even]))
				break
			}
			if time.Now().After(deadline) {
				app.Logger.Error(fmt.Sprintf(
					"compat: CHURN FAIL window=%d id=%d still in manager after close", i, id))
				return
			}
			time.Sleep(50 * time.Millisecond)
		}
	}
	app.Logger.Info("compat: CHURN PASS")
}

func runSuite(app *application.App, win *application.WebviewWindow) {
	settle := func() { time.Sleep(300 * time.Millisecond) }
	// waitUntil tolerates asynchronous window-state transitions — macOS
	// animates minimise/fullscreen, so the state lands well after the
	// call returns
	waitUntil := func(cond func() bool, timeout time.Duration) {
		deadline := time.Now().Add(timeout)
		for !cond() && time.Now().Before(deadline) {
			time.Sleep(100 * time.Millisecond)
		}
	}

	// size
	win.SetSize(900, 700)
	settle()
	w, h := win.Size()
	sizeOK := w == 900 && h == 700
	if runtime.GOOS == "darwin" {
		// mac window chrome math differs (title bar participates in
		// bounds differently than linux/windows); accept the round-trip
		// within a small band and log the delta for analysis
		sizeOK = w == 900 && h >= 660 && h <= 700
	}
	check(win, app, "size", sizeOK, fmt.Sprintf("expect=900x700 got=%dx%d", w, h))

	// always on top (no public readback; exercised for crash/behaviour only)
	win.SetAlwaysOnTop(true)
	settle()
	win.SetAlwaysOnTop(false)
	settle()
	check(win, app, "always-on-top", true, "toggled without error")

	// maximise
	win.Maximise()
	settle()
	maximised := win.IsMaximised()
	win.UnMaximise()
	settle()
	check(win, app, "maximise", maximised && !win.IsMaximised(), fmt.Sprintf("maximised=%v", maximised))

	// minimise
	win.Minimise()
	waitUntil(func() bool { return win.IsMinimised() }, 3*time.Second)
	minimised := win.IsMinimised()
	win.Restore()
	waitUntil(func() bool { return !win.IsMinimised() }, 3*time.Second)
	check(win, app, "minimise", minimised && !win.IsMinimised(), fmt.Sprintf("minimised=%v", minimised))

	// fullscreen
	win.Fullscreen()
	waitUntil(func() bool { return win.IsFullscreen() }, 4*time.Second)
	full := win.IsFullscreen()
	win.UnFullscreen()
	waitUntil(func() bool { return !win.IsFullscreen() }, 4*time.Second)
	check(win, app, "fullscreen", full && !win.IsFullscreen(), fmt.Sprintf("fullscreen=%v", full))

	// visibility
	win.Hide()
	settle()
	hidden := !win.IsVisible()
	win.Show()
	settle()
	check(win, app, "visibility", hidden && win.IsVisible(), fmt.Sprintf("hidden=%v visible=%v", hidden, win.IsVisible()))

	// ignore mouse events
	win.SetIgnoreMouseEvents(true)
	settle()
	ignoring := win.IsIgnoreMouseEvents()
	win.SetIgnoreMouseEvents(false)
	settle()
	check(win, app, "ignore-mouse", ignoring && !win.IsIgnoreMouseEvents(), fmt.Sprintf("ignoring=%v", ignoring))

	// zoom
	win.SetZoom(1.5)
	settle()
	check(win, app, "zoom", true, "set without error")
	win.SetZoom(1.0)
}
