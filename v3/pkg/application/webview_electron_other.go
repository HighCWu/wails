//go:build !linux

package application

// Electron window dispatch and the loopback asset base URL override are
// Linux-only for now; other platforms keep their native webview paths.
func init() {}
