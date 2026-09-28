package application

// Optional native-engine hooks. Only wails_cef platform files install them.
var preparePlatformCEF = func(*App) error { return nil }
var startPlatformCEF = func(*App) error { return nil }
var stopPlatformCEF = func() {}

type cefDesktopEngine interface {
	execJS(string)
	loadURL(string)
	reload(bool)
	setZoomFactor(float64)
	zoomFactor() float64
	close()
	focus()
	openDevTools()
}
