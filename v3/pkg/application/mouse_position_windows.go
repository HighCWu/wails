//go:build windows

package application

import "github.com/wailsapp/wails/v3/pkg/w32"

func init() {
	MousePositionFunc = func() (int, int, bool) {
		return w32.GetCursorPos()
	}
}
