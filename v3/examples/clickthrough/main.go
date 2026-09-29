package main

import (
	"embed"
	"log"
	"time"

	"github.com/wailsapp/wails/v3/pkg/application"
	"github.com/wailsapp/wails/v3/pkg/events"
)

//go:embed assets
var assets embed.FS

// hitMask is the per-pixel hit grid uploaded by the frontend: the page is
// rasterised offscreen and sampled into cell-sized blocks; bit i of bits is
// 1 when block i is opaque (interactive).
type hitMask struct {
	cell int    // CSS pixels per grid cell
	w    int    // cells across
	h    int    // cells down
	bits []byte // packed row-major, bit index = y*w + x
}

func (m *hitMask) interactive(x, y int) bool {
	if x < 0 || y < 0 || x >= m.w*m.cell || y >= m.h*m.cell {
		return false
	}
	gx, gy := x/m.cell, y/m.cell
	idx := gy*m.w + gx
	return m.bits[idx/8]&(1<<(idx%8)) != 0
}

// engine implements the polling hit-test loop for the transparent overlay:
// the overlay toggles whole-window mouse passthrough as the cursor moves
// between transparent and opaque regions.
//
// Design rules distilled from a real-world transparent hit-test project
// (see examples/clickthrough/README.md):
//  1. Hit-testing must run natively (this goroutine), never from JS
//     mousemove: an ignored window receives no mouse events and would lock
//     itself in the passthrough state forever.
//  2. Toggle whole-window passthrough instead of X11 input shapes; shape
//     regions survive resizes on the X server and leak hit-test holes.
//  3. Fail open: any error (cursor unreadable, no mask, unsupported
//     platform) falls back to the fully interactive window.
type engine struct {
	app    *application.App
	window *application.WebviewWindow

	mode       string // "off" | "alpha" | "ignore"
	mask       *hitMask
	lastIgnore bool
}

func (e *engine) setMode(mode string) {
	switch mode {
	case "alpha", "ignore", "off":
		e.mode = mode
		e.app.Logger.Info("clickthrough: mode", "mode", mode)
		e.app.Event.Emit("ct:state", map[string]any{"mode": mode, "ignoring": e.lastIgnore})
	}
}

func (e *engine) setMask(data map[string]any) {
	cell, _ := data["cell"].(float64)
	w, _ := data["w"].(float64)
	h, _ := data["h"].(float64)
	rawBits, _ := data["bits"].([]any)
	if cell <= 0 || w <= 0 || h <= 0 || len(rawBits) == 0 {
		e.app.Logger.Info("clickthrough: invalid mask rejected")
		return
	}
	bits := make([]byte, len(rawBits))
	for i, v := range rawBits {
		f, _ := v.(float64)
		bits[i] = byte(f)
	}
	e.mask = &hitMask{cell: int(cell), w: int(w), h: int(h), bits: bits}
	e.app.Logger.Info("clickthrough: mask uploaded",
		"cell", int(cell), "w", int(w), "h", int(h), "bytes", len(bits))
}

// run polls the global cursor position and flips window passthrough on
// state changes. Runs on its own goroutine; window access goes through
// WebviewWindow methods, which marshal to the main thread.
func (e *engine) run() {
	ticker := time.NewTicker(16 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-e.app.Context().Done():
			return
		case <-ticker.C:
		}

		mode, mask := e.mode, e.mask
		var ignore bool
		switch mode {
		case "ignore":
			ignore = true
		case "alpha":
			if mask == nil {
				ignore = false // fail open until the frontend uploads a mask
			} else {
				ignore = !e.hitTest(mask)
			}
		default: // "off"
			ignore = false
		}

		if ignore != e.lastIgnore {
			e.lastIgnore = ignore
			e.window.SetIgnoreMouseEvents(ignore)
			e.app.Logger.Info("clickthrough: flip", "ignoring", ignore, "mode", mode)
			e.app.Event.Emit("ct:state", map[string]any{"mode": mode, "ignoring": ignore})
		}
	}
}

// hitTest maps the global cursor into overlay-local coordinates and looks
// up the mask. Scale caveat: window Position() is logical while
// MousePosition() is physical; at scale factor 1 they coincide (Xvfb CI and
// typical Linux setups). Mixed-DPI handling is future work.
func (e *engine) hitTest(mask *hitMask) bool {
	if mask == nil {
		return false
	}
	x, y, ok := application.MousePosition()
	if !ok {
		return false
	}
	wx, wy := e.window.Position()
	return mask.interactive(x-wx, y-wy)
}

func main() {
	app := application.New(application.Options{
		Name: "clickthrough",
		Assets: application.AssetOptions{
			Handler: application.BundledAssetFileServer(assets),
		},
	})

	// The underlay sits below the overlay and counts clicks that pass
	// through the overlay's transparent regions.
	app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name:   "underlay",
		Title:  "Underlay (pass-through click counter)",
		X:      260,
		Y:      260,
		Width:  700,
		Height: 500,
		URL:    "/underlay.html",
	})

	overlay := app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name:           "overlay",
		Title:          "Overlay (transparent hit-test)",
		X:              100,
		Y:              100,
		Width:          480,
		Height:         640,
		Frameless:      true,
		AlwaysOnTop:    true,
		DisableResize:  true,
		BackgroundType: application.BackgroundTypeTransparent,
		URL:            "/",
	})

	// default matches the overlay UI's pre-selected "Alpha mask" button;
	// until a mask arrives alpha mode fails open (fully interactive)
	engine := &engine{app: app, window: overlay, mode: "alpha"}

	app.Event.On("ct:mode", func(e *application.CustomEvent) {
		if mode, ok := e.Data.(string); ok {
			engine.setMode(mode)
		}
	})
	app.Event.On("ct:mask", func(e *application.CustomEvent) {
		if data, ok := e.Data.(map[string]any); ok {
			engine.setMask(data)
		}
	})
	app.Event.OnApplicationEvent(events.Common.ApplicationStarted, func(*application.ApplicationEvent) {
		go engine.run()
	})

	err := app.Run()
	if err != nil {
		log.Fatal(err)
	}
}
