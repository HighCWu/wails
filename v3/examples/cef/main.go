//go:build linux || windows || (darwin && !ios)

// This example exercises the CEF backend and can also run with the system webview.
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

type ProbeService struct{}

func (*ProbeService) Greet(name string) string {
	fmt.Printf("CEF_SMOKE_GREET %q\n", name)
	return "Hello, " + name + "!"
}

func (*ProbeService) Report(message string) {
	fmt.Printf("CEF_SMOKE_REPORT %s\n", message)
}

func (*ProbeService) Quit() {
	go func() {
		delay := 200 * time.Millisecond
		if os.Getenv("CEF_SMOKE_DELAY_QUIT") == "1" {
			delay = 5 * time.Second
		}
		time.Sleep(delay)
		application.Get().Quit()
	}()
}
func (*ProbeService) Resize() {
	if w, ok := application.Get().Window.GetByName("main"); ok {
		w.SetSize(1000, 700)
	}
}

func (*ProbeService) NewWindow() {
	w := newWindow(application.Get(), "secondary", false, application.PermissionDeny)
	w.SetPosition(20, 40)
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
	d.AddButton("Yes").SetAsDefault().OnClick(func() { fmt.Printf("CEF_SMOKE_DIALOG accepted attached=%t\n", attached) })
	d.AddButton("No").SetAsCancel()
	fmt.Printf("CEF_SMOKE_DIALOG opened attached=%t\n", attached)
	d.Show()
}

func newWindow(app *application.App, name string, frameless bool, permission application.Permission) *application.WebviewWindow {
	win := app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name: name, Title: "CEF smoke " + name, URL: "/?role=" + name + "&passive=" + os.Getenv("CEF_SMOKE_PASSIVE"), Width: 900, Height: 640,
		Frameless: frameless, EnableFileDrop: name == "main", DevToolsEnabled: true,
		Permissions: map[application.PermissionType]application.Permission{
			application.PermissionCamera: permission, application.PermissionMicrophone: permission,
		},
		KeyBindings: map[string]func(application.Window){
			"Ctrl+M": func(window application.Window) {
				fmt.Printf("CEF_SMOKE_EXECJS %s\n", name)
				window.ExecJS(`document.getElementById("output").textContent = "Go ExecJS OK"; document.title = "CEF smoke ExecJS OK"`)
			},
		},
	})
	win.OnWindowEvent(events.Common.WindowFilesDropped, func(event *application.WindowEvent) {
		fmt.Printf("CEF_SMOKE_DROP %s %q %#v\n", name, event.Context().DroppedFiles(), event.Context().DropTargetDetails())
	})
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
