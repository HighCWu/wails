//go:build linux && cgo && !android && !server

package application

func init() {
	// getMousePosition queries GDK and must run on the main thread; both
	// GTK build variants (linux_cgo.go / linux_cgo_gtk3.go) provide it.
	MousePositionFunc = func() (int, int, bool) {
		var x, y int
		InvokeSync(func() {
			x, y, _ = getMousePosition()
		})
		return x, y, true
	}
}
