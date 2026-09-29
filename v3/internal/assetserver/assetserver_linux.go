//go:build linux && !android

package assetserver

import "net/url"

var baseURL = url.URL{
	Scheme: "wails",
	Host:   "localhost",
}

// SetBaseURL overrides the platform default base URL. It exists for
// alternative webview backends: the electron backend serves the asset
// server from a loopback HTTP listener instead of the custom wails://
// scheme. Must be called before GetStartURL.
func SetBaseURL(scheme, host string) {
	baseURL = url.URL{Scheme: scheme, Host: host}
}
