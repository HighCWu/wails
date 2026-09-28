//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios

// cefhelper runs CEF renderer, GPU and utility subprocesses without importing
// the host UI toolkit. On macOS, package it in the standard CEF Helper bundles.
package main

import (
	"fmt"
	"os"
	"runtime"

	"github.com/wailsapp/wails/v3/internal/cef"
)

func init() { runtime.LockOSThread() }

func main() {
	if !cef.IsSubprocess() {
		fmt.Fprintln(os.Stderr, "cefhelper must be launched by CEF as a subprocess")
		os.Exit(1)
	}
	if err := cef.ExecuteSubprocess(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
