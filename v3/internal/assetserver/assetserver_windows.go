package assetserver

import "net/url"

var baseURL = url.URL{
	Scheme: "http",
	Host:   "wails.localhost",
}

// SetBaseURL overrides the platform default base URL. It exists for
// alternative webview backends: the electron backend serves the asset
// server from a loopback HTTP listener instead of the wails.localhost
// host. Must be called before GetStartURL.
func SetBaseURL(scheme, host string) {
	baseURL = url.URL{Scheme: scheme, Host: host}
}
