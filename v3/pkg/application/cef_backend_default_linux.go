//go:build linux && cgo && !wails_cef && !android && !server

package application

// No-op CEF hooks for builds without the wails_cef tag: the backend
// resolver sees no CEF probe, auto resolves to the system webview, and a
// forced "cef" fails at Run() with a descriptive error before any of
// these hooks could matter. Kept as real functions so shared files
// (webview_window_linux.go, application_linux*.go, linux_cgo*.go) compile
// identically with and without the tag.

func initCEFBackend(app *App) error { return nil }

func shutdownCEFBackend() {}

func (w *linuxWebviewWindow) attachCEFEngine() {}
