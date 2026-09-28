//go:build linux && gtk3

// This example exercises the CEF backend and can also run with the system webview.
package main

import (
	"embed"
	"fmt"
	"log"

	"github.com/wailsapp/wails/v3/pkg/application"
)

//go:embed assets
var assets embed.FS

type ProbeService struct{}

func (*ProbeService) Greet(name string) string {
	fmt.Printf("CEF_SMOKE_GREET %q\n", name)
	return "Hello, " + name + "!"
}

func main() {
	app := application.New(application.Options{
		Name:     "cef-smoke",
		Services: []application.Service{application.NewService(&ProbeService{})},
		Assets:   application.AssetOptions{Handler: application.BundledAssetFileServer(assets)},
	})
	app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name: "main", Title: "CEF smoke", URL: "/", Width: 900, Height: 640,
		DevToolsEnabled: true,
		KeyBindings: map[string]func(application.Window){
			"Ctrl+M": func(window application.Window) {
				fmt.Println("CEF_SMOKE_EXECJS")
				window.ExecJS(`document.getElementById("output").textContent = "Go ExecJS OK"; document.title = "CEF smoke ExecJS OK"`)
			},
		},
	})

	if err := app.Run(); err != nil {
		log.Fatal(err)
	}
	fmt.Println("CEF_SMOKE_EXIT")
}
