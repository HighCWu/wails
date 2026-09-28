//go:build windows && cgo && wails_cef

package cef

/*
void wcef_set_osmodal_loop(int active);
*/
import "C"

// SetOSModalLoop preserves Windows dialog/menu keyboard processing while CEF
// shares the native UI thread. Call before and after a native modal loop.
func SetOSModalLoop(active bool) {
	if Initialized() {
		C.wcef_set_osmodal_loop(gtkBoolC(active))
	}
}
