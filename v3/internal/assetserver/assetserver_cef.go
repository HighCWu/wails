//go:build wails_cef && (windows || (darwin && !ios))

package assetserver

import "net/url"

// SetBaseURL selects the standard HTTP origin used by CEF before navigation.
func SetBaseURL(scheme, host string) { baseURL = url.URL{Scheme: scheme, Host: host} }
