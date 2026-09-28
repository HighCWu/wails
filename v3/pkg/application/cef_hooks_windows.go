//go:build windows && !server

package application

var attachCEFWindows = func(*windowsWebviewWindow) bool { return false }

var enterCEFModalLoop = func() func() { return func() {} }
