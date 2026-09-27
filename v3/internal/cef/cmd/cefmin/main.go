// Command cefmin is a minimal browser-process smoke test for the cef
// package, bypassing the wails application layer.
package main

import (
	"fmt"
	"os"

	"github.com/wailsapp/wails/v3/internal/cef"
)

func main() {
	if cef.IsSubprocess() {
		if err := cef.ExecuteSubprocess(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	cef.SetStateHooks(&cef.State{})
	err := cef.Initialize(cef.InitializeOptions{Dir: os.Getenv("WAILS_CEF_DIR")})
	fmt.Println("Initialize:", err)
	if err == nil {
		cef.Shutdown()
	}
}
