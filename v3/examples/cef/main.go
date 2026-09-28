//go:build linux && gtk3

// This example exercises the CEF backend and can also run with the system webview.
package main

import (
	"embed"
	"fmt"
	"github.com/wailsapp/wails/v3/pkg/events"
	"log"
	"os"
	"time"

	"github.com/wailsapp/wails/v3/pkg/application"
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

func (*ProbeService) NewWindow() {
	newWindow(application.Get(), "secondary", false, application.PermissionDeny)
}

func newWindow(app *application.App, name string, frameless bool, permission application.Permission) {
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
