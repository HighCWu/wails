//go:build linux

package application

import "github.com/wailsapp/wails/v3/internal/assetserver"

func init() {
	// The electron backend serves the asset server from a loopback HTTP
	// listener; redirect URL resolution before any window resolves its
	// start URL (mirrors the CEF backend's http://wails.localhost choice).
	platformSetAssetBaseURL = assetserver.SetBaseURL
}
