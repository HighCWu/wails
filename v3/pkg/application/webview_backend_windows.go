//go:build windows

package application

import (
	"github.com/wailsapp/wails/v3/internal/webview2/webviewloader"
)

// The windows system webview (WebView2) is a separately installed
// runtime — probe for it instead of assuming. Win10 LTSC, Server SKUs
// and stripped images frequently ship without one, which is exactly the
// signal the auto backend decision (and third-party launchers deciding
// whether to provision an Electron runtime) needs.
func init() {
	systemWebviewAvailable = func() bool {
		// An empty browserPath uses the standard WebView2 search
		// (evergreen bootstrapper / fixed-version paths). A
		// fixed-version deployment configured through
		// Options.Windows.WebviewBrowserPath is honoured later at window
		// creation; this pre-flight probe covers the standard install.
		version, err := webviewloader.GetAvailableCoreWebView2BrowserVersionString("")
		return err == nil && version != ""
	}
}
