//go:build linux && !android && !server

package application

import (
	"errors"
	"sync"
)

func (a *linuxApp) processAndCacheScreens() error {
	// Electron backend: getScreens is a GDK call; with the electron
	// backend's inline main-thread dispatch it would run on an arbitrary
	// goroutine against the GTK main thread's heap (heap corruption).
	// Electron windows report screens through the control protocol once
	// implemented; until then screens are unavailable.
	if a.parent != nil && a.parent.webviewBackend == WebviewBackendElectron {
		return errors.New("screens are not available on the electron backend yet")
	}
	var wg sync.WaitGroup
	var screens []*Screen
	var err error
	wg.Add(1)
	InvokeSync(func() {
		screens, err = getScreens(a.application)
		wg.Done()
	})
	wg.Wait()
	if err != nil {
		return err
	}
	// gdk_monitor_is_primary is unreliable on Wayland (always returns false).
	// If no screen reports as primary, default to index 0.
	hasPrimary := false
	for _, s := range screens {
		if s.IsPrimary {
			hasPrimary = true
			break
		}
	}
	if !hasPrimary && len(screens) > 0 {
		screens[0].IsPrimary = true
	}
	return a.parent.Screen.LayoutScreens(screens)
}

func (a *linuxApp) getPrimaryScreen() (*Screen, error) {
	return a.parent.Screen.GetPrimary(), nil
}

func (a *linuxApp) getScreens() ([]*Screen, error) {
	return a.parent.Screen.GetAll(), nil
}

func getScreenForWindow(window *linuxWebviewWindow) (*Screen, error) {
	return window.getScreen()
}
