//go:build linux && cgo && !gtk3 && !android && !server

package application

// CEF backend stubs for the GTK4/WebKitGTK-6.0 variant: the CEF engine is
// only implemented for the GTK3 variant so far. probeCEFRuntime stays nil,
// so "auto" resolves to the system webview and forcing "cef" fails at
// startup with a descriptive error instead of silently falling back.

func initCEFBackend(app *App) error {
	if app.webviewBackend == WebviewBackendCEF {
		app.webviewBackendError = errCEFUnsupported
		return errCEFUnsupported
	}
	return nil
}

func shutdownCEFBackend() {}

func (w *linuxWebviewWindow) attachCEFEngine() {}

var errCEFUnsupported = cefUnsupportedError{}

type cefUnsupportedError struct{}

func (cefUnsupportedError) Error() string {
	return "webview backend \"cef\" is only available in GTK3 builds on Linux — rebuild with -tags gtk3"
}
