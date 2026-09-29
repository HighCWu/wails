package main

import (
	"embed"
	"fmt"
	"log"
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

	app.Event.OnApplicationEvent(events.Common.ApplicationStarted, func(*application.ApplicationEvent) {
		go func() {
			app.Logger.Info(fmt.Sprintf("compat: backend=%s", app.WebviewBackend()))
			time.Sleep(1 * time.Second) // let the page finish loading
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
