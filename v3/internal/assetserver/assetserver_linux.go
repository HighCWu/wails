//go:build linux && !android

package assetserver

import "net/url"

var baseURL = url.URL{
	Scheme: "wails",
	Host:   "localhost",
}

// SetBaseURL overrides the platform default base URL. It exists for the
// CEF webview backend, which serves the asset server from
// http://wails.localhost (matching the Windows WebView2 backend) instead
// of the custom wails:// scheme, avoiding Chromium's non-special-scheme
// URL parsing restrictions. Must be called before GetStartURL.
func SetBaseURL(scheme, host string) {
	baseURL = url.URL{Scheme: scheme, Host: host}
}
