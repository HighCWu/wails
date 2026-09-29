package application

// MousePositionFunc reports the global screen cursor position in physical
// (device) coordinates. It is installed by platform glue (see
// mouse_position_linux.go / mouse_position_windows.go) and deliberately kept
// indirect so this file carries no platform-specific dependencies and the
// default fails closed: callers must treat ok == false as "unsupported" and
// degrade (e.g. keep the window fully interactive).
var MousePositionFunc = func() (x, y int, ok bool) {
	return 0, 0, false
}

// MousePosition returns the global cursor position in physical screen
// coordinates. ok is false on platforms where the position is unavailable
// (unsupported platform, X11 connection failure, non-interactive session).
//
// This backs native polling hit-testing for transparent windows: once a
// window ignores mouse events it no longer receives mousemove, so the only
// way to detect the cursor re-entering an opaque region is to poll the OS
// cursor position from the host.
func MousePosition() (int, int, bool) {
	return MousePositionFunc()
}
