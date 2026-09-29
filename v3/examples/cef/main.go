//go:build linux || windows || (darwin && !ios)

// This example exercises the CEF backend and can also run with the system webview.
package main

import (
	"embed"
	"fmt"
	"log"
	"os"
	"runtime"
	"time"

	"github.com/wailsapp/wails/v3/pkg/application"
	"github.com/wailsapp/wails/v3/pkg/events"
	"github.com/wailsapp/wails/v3/pkg/icons"
)

//go:embed assets
var assets embed.FS

type ProbeService struct{}

func (*ProbeService) Greet(name string) string {
	fmt.Printf("CEF_SMOKE_GREET %q\n", name)
	return "Hello, " + name + "!"
}

func (*ProbeService) Report(message string) {
	fmt.Printf("CEF_SMOKE_REPORT %s\n", message)
}

func (*ProbeService) Quit() {
	fmt.Println("CEF_SMOKE_QUIT requested")
	go func() {
		delay := 200 * time.Millisecond
		if os.Getenv("CEF_SMOKE_DELAY_QUIT") == "1" {
			delay = 5 * time.Second
		}
		time.Sleep(delay)
		fmt.Println("CEF_SMOKE_QUIT dispatch")
		application.Get().Quit()
	}()
}
func (*ProbeService) Resize() {
	if w, ok := application.Get().Window.GetByName("main"); ok {
		w.SetSize(1000, 700)
	}
}

// WindowLifecycle follows the minimise/restore and hide/show examples. Drive
// restoration from Go so an occluded renderer's timer cannot make it pass.
func (*ProbeService) WindowLifecycle() error {
	w, ok := application.Get().Window.GetByName("main")
	if !ok {
		return fmt.Errorf("main window missing")
	}
	wait := func(name string, query func() bool, expected bool) error {
		deadline := time.Now().Add(10 * time.Second)
		for time.Now().Before(deadline) {
			if query() == expected {
				fmt.Printf("CEF_SMOKE_WINDOW %s\n", name)
				return nil
			}
			time.Sleep(50 * time.Millisecond)
		}
		return fmt.Errorf("%s did not reach %t", name, expected)
	}
	w.Minimise()
	if err := wait("minimised", w.IsMinimised, true); err != nil {
		return err
	}
	w.UnMinimise()
	if err := wait("restored", w.IsMinimised, false); err != nil {
		return err
	}
	w.Hide()
	if err := wait("hidden", w.IsVisible, false); err != nil {
		return err
	}
	w.Show()
	return wait("shown", w.IsVisible, true)
}

func (*ProbeService) NewWindow() {
	w := newWindow(application.Get(), "secondary", false, application.PermissionDeny)
	w.SetPosition(20, 40)
}

// stabReady carries per-window readiness signals for Stability: the stab
// page calls StabReady through its own bridge once loaded, proving the
// freshly created window's RPC path works.
var stabReady = map[string]chan struct{}{}

func (*ProbeService) StabReady(name string) {
	fmt.Printf("CEF_SMOKE_STAB ready %s\n", name)
	if ch, ok := stabReady[name]; ok {
		select {
		case ch <- struct{}{}:
		default:
		}
	}
}

// Stability runs create/ready/close churn in a goroutine and logs each
// step for the isolated runners to assert. Even cycles wait for the page
// to announce readiness before closing; odd cycles close while the page
// is still loading (the pending-creation path). The final live-browsers
// assertion in the runners covers CEF-side cleanup, which this loop
// deliberately does not measure itself.
func (*ProbeService) Stability(cycles int) {
	app := application.Get()
	go func() {
		for i := 0; i < cycles; i++ {
			name := fmt.Sprintf("stab-%d", i)
			fmt.Printf("CEF_SMOKE_STAB create %s\n", name)
			w := newWindow(app, name, false, application.PermissionDeny)
			if i%2 == 0 {
				ch := make(chan struct{}, 1)
				stabReady[name] = ch
				select {
				case <-ch:
				case <-time.After(15 * time.Second):
					fmt.Printf("CEF_SMOKE_STAB timeout %s\n", name)
				}
				delete(stabReady, name)
			} else {
				// Still loading: exercise the pending-creation close path.
				time.Sleep(150 * time.Millisecond)
			}
			w.Close()
			deadline := time.Now().Add(10 * time.Second)
			for time.Now().Before(deadline) {
				if _, ok := app.Window.GetByName(name); !ok {
					break
				}
				time.Sleep(50 * time.Millisecond)
			}
			_, stillThere := app.Window.GetByName(name)
			fmt.Printf("CEF_SMOKE_STAB closed %s removed=%t\n", name, !stillThere)
		}
		fmt.Printf("CEF_SMOKE_STAB done cycles=%d\n", cycles)
	}()
}

// Overlay probes whole-window mouse passthrough, independently of alpha rendering.
func (*ProbeService) Overlay(ignore bool) {
	app := application.Get()
	if w, ok := app.Window.GetByName("overlay"); ok {
		w.SetIgnoreMouseEvents(ignore)
		return
	}
	w := app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name: "overlay", Title: "CEF smoke overlay", URL: "/?role=overlay", Width: 400, Height: 260,
		Frameless: true, AlwaysOnTop: true, IgnoreMouseEvents: ignore,
	})
	if main, ok := app.Window.GetByName("main"); ok {
		x, y := main.Position()
		w.SetPosition(x+40, y+60)
	}
}
func (*ProbeService) CloseOverlay() {
	if w, ok := application.Get().Window.GetByName("overlay"); ok {
		w.Close()
	}
}

// Dialog exercises both a top-level native dialog and an attached sheet/modal.
func (*ProbeService) Dialog(attached bool) {
	d := application.Get().Dialog.Question().SetTitle("CEF smoke dialog").SetMessage("CEF native dialog probe")
	if attached {
		if w, ok := application.Get().Window.GetByName("main"); ok {
			d.AttachToWindow(w)
		}
	}
	yes := d.AddButton("Yes").OnClick(func() { fmt.Printf("CEF_SMOKE_DIALOG accepted attached=%t\n", attached) })
	no := d.AddButton("No").SetAsCancel().OnClick(func() { fmt.Printf("CEF_SMOKE_DIALOG rejected attached=%t\n", attached) })
	if os.Getenv("CEF_SMOKE_UPSTREAM") == "1" {
		// Mirrors examples/dialogs: a non-first default and custom icon.
		no.SetAsDefault()
		d.SetIcon(icons.ApplicationDarkMode256)
	} else {
		yes.SetAsDefault()
	}
	fmt.Printf("CEF_SMOKE_DIALOG opened attached=%t\n", attached)
	go func() {
		time.Sleep(250 * time.Millisecond)
		application.InvokeAsync(func() { fmt.Printf("CEF_SMOKE_DIALOG dispatch attached=%t\n", attached) })
	}()
	d.Show()
}

func newWindow(app *application.App, name string, frameless bool, permission application.Permission) *application.WebviewWindow {
	win := app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name: name, Title: "CEF smoke " + name, URL: "/?role=" + name + "&passive=" + os.Getenv("CEF_SMOKE_PASSIVE") + "&platform=" + runtime.GOOS, Width: 900, Height: 640,
		Frameless: frameless, EnableFileDrop: name == "main", DevToolsEnabled: true,
		Permissions: map[application.PermissionType]application.Permission{
			application.PermissionCamera: permission, application.PermissionMicrophone: permission,
		},
		KeyBindings: map[string]func(application.Window){
			"F9": func(window application.Window) {
				window.ExecJS("window.cefCheckFileDialog()")
			},
			"F8": func(window application.Window) {
				fmt.Println("CEF_SMOKE_WINDOW_API requested")
				window.ExecJS("window.cefCheckUpstreamWindowAPI()")
			},
			"Ctrl+M": func(window application.Window) {
				fmt.Printf("CEF_SMOKE_EXECJS %s\n", name)
				window.ExecJS(`document.getElementById("output").textContent = "Go ExecJS OK"; document.title = "CEF smoke ExecJS OK"`)
			},
		},
	})
	win.OnWindowEvent(events.Common.WindowFilesDropped, func(event *application.WindowEvent) {
		fmt.Printf("CEF_SMOKE_DROP %s %q %#v\n", name, event.Context().DroppedFiles(), event.Context().DropTargetDetails())
	})
	win.OnWindowEvent(events.Common.WindowRenderCrash, func(*application.WindowEvent) {
		fmt.Printf("CEF_SMOKE_CRASH_EVENT %s\n", name)
	})
	if runtime.GOOS == "darwin" {
		win.OnWindowEvent(events.Mac.WindowDidEnterFullScreen, func(*application.WindowEvent) {
			win.ExecJS("window.cefNativeFullscreen = true")
		})
		win.OnWindowEvent(events.Mac.WindowDidExitFullScreen, func(*application.WindowEvent) {
			win.ExecJS("window.cefNativeFullscreen = false")
		})
	}
	return win
}

func main() {
	passive := os.Getenv("CEF_SMOKE_PASSIVE") == "1"
	if passive {
		configurePassiveWindows()
	}
	app := application.New(application.Options{
		Name:     "cef-smoke",
		Services: []application.Service{application.NewService(&ProbeService{})},
		Assets:   application.AssetOptions{Handler: application.BundledAssetFileServer(assets)},
	})

	newWindow(app, "main", os.Getenv("CEF_SMOKE_FRAMELESS") == "1", application.PermissionAllow)

	if passive {
		go func() { time.Sleep(15 * time.Second); app.Quit() }()
	}
	if err := app.Run(); err != nil {
		log.Fatal(err)
	}
	fmt.Println("CEF_SMOKE_EXIT")
}
