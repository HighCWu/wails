//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios && !server

package application

import (
	"github.com/wailsapp/wails/v3/internal/assetserver/webview"
	"github.com/wailsapp/wails/v3/internal/cef"
)

// Keep native WebKit/WebView dependencies out of CEF helper processes.
type cefAssetRequest struct{ *cef.Request }

func (r cefAssetRequest) Response() webview.ResponseWriter { return r.Request.Response() }

var _ webview.Request = cefAssetRequest{}
