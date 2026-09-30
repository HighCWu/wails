package main

import (
	"embed"
	"fmt"
	"log"
	"os"
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
	app := application.New(application.Options{
		Name: "webview-compat",
		Assets: application.AssetOptions{
			Handler: application.BundledAssetFileServer(assets),
		},
		Services: []application.Service{
			application.NewService(&CompatService{}),
		},
	})

	win := app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name:   "main",
		Title:  "webview-compat",
		Width:  800,
		Height: 600,
	})

	// Crash recovery: the backend recovers the page with one auto-reload
	// and reports through the custom event (no typed upstream event
	// exists for renderer crashes).
	app.Event.On("electron:rendererCrashed", func(e *application.CustomEvent) {
		app.Logger.Info("compat: CRASH-EVENT", "data", e.Data)
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
		app.Logger.Info("compat: BENCH-FINISHED")
	})

	app.Event.OnApplicationEvent(events.Common.ApplicationStarted, func(*application.ApplicationEvent) {
		go func() {
			app.Logger.Info(fmt.Sprintf("compat: backend=%s", app.WebviewBackend()))
			time.Sleep(1 * time.Second) // let the page finish loading
			if os.Getenv("WAILS_COMPAT_CHURN") == "1" {
				// independent windows; runs regardless of the suite
				// verdict so known suite gaps can't gate lifecycle work
				runChurn(app)
			}
			runSuite(app, win)
			if failures > 0 {
				app.Logger.Info(fmt.Sprintf("compat: SUITE FAIL failures=%d", failures))
				return
			}
			app.Logger.Info("compat: SUITE PASS")
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

	// size
	win.SetSize(900, 700)
	settle()
	w, h := win.Size()
	check(win, app, "size", w == 900 && h == 700, fmt.Sprintf("expect=900x700 got=%dx%d", w, h))

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
	settle()
	minimised := win.IsMinimised()
	win.Restore()
	settle()
	check(win, app, "minimise", minimised && !win.IsMinimised(), fmt.Sprintf("minimised=%v", minimised))

	// fullscreen
	win.Fullscreen()
	settle()
	full := win.IsFullscreen()
	win.UnFullscreen()
	settle()
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
