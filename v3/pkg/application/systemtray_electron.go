package application

// Electron backend implementation of systemTrayImpl: each tray maps to an
// Electron Tray in the host process, built through the pure-JS helper in
// main.js (same path as the menus — buildFromTemplate from the addon does
// not render on linux). Clicks arrive as tray-* events on the control
// plane; menu item clicks carry the item uid like the window menubar.

import (
	"encoding/base64"
	"fmt"
)

// blankPNG is a 16x16 transparent icon: the Tray constructor needs an
// image, and wails allows creating a tray before SetIcon.
const blankPNG = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAEklEQVR4nGNgGAWjYBSMAggAAAQQAAFVN1rQAAAAAElFTkSuQmCC"

type electronSystemTray struct {
	parent *SystemTray
	id     uint
	hidden bool
}

// electronTrayState carries what the event pump needs to route tray
// events back to the wails object.
type electronTrayState struct {
	parent *SystemTray
	menu   *Menu
}

func newElectronSystemTray(s *SystemTray) systemTrayImpl {
	return &electronSystemTray{parent: s, id: s.id}
}

func pngDataURL(icon []byte) string {
	if len(icon) == 0 {
		return blankPNG
	}
	return "data:image/png;base64," + base64.StdEncoding.EncodeToString(icon)
}

func (t *electronSystemTray) electronTrayCall(method string, params map[string]any, out any) error {
	return electronProcessCall(method, 0, params, out)
}

func (t *electronSystemTray) spec(action string) map[string]any {
	return map[string]any{"id": t.id, "action": action}
}

func (t *electronSystemTray) run() {
	t.create()
}

// create (re)creates the Electron Tray from the parent's current state.
func (t *electronSystemTray) create() {
	electronBackend.mu.Lock()
	if electronBackend.trays == nil {
		electronBackend.trays = make(map[uint]*electronTrayState)
	}
	state := &electronTrayState{parent: t.parent, menu: t.parent.menu}
	electronBackend.trays[t.id] = state
	electronBackend.mu.Unlock()
	params := t.spec("create")
	params["icon"] = pngDataURL(t.parent.icon)
	if t.parent.tooltip != "" {
		params["tooltip"] = t.parent.tooltip
	}
	if t.parent.menu != nil {
		params["menu"] = electronSerializeMenu(t.parent.menu.items)
	}
	if err := t.electronTrayCall("trayOp", params, nil); err != nil {
		globalApplication.error("electron: tray create: %v", err)
	}
}

func (t *electronSystemTray) setLabel(label string) {
	// Electron's setTitle is macOS-only; nothing to map on linux/windows
	// (the linux native tray maps it to the StatusNotifier Title).
}

func (t *electronSystemTray) setTooltip(tooltip string) {
	params := t.spec("settooltip")
	params["tooltip"] = tooltip
	if err := t.electronTrayCall("trayOp", params, nil); err != nil {
		globalApplication.error("electron: tray setTooltip: %v", err)
	}
}

func (t *electronSystemTray) setIcon(icon []byte) {
	params := t.spec("seticon")
	params["icon"] = pngDataURL(icon)
	if err := t.electronTrayCall("trayOp", params, nil); err != nil {
		globalApplication.error("electron: tray setIcon: %v", err)
	}
}

func (t *electronSystemTray) setTemplateIcon(icon []byte) {
	// macOS template icons have no linux/windows equivalent
}

func (t *electronSystemTray) setDarkModeIcon(icon []byte) {
	// macOS dark-mode variant has no linux/windows equivalent
}

func (t *electronSystemTray) setIconPosition(position IconPosition) {
	// the Electron tray has no icon+label composition to position
}

func (t *electronSystemTray) setMenu(menu *Menu) {
	electronBackend.mu.Lock()
	if state := electronBackend.trays[t.id]; state != nil {
		state.menu = menu
	}
	electronBackend.mu.Unlock()
	attachElectronMenuImpls(menu, &electronMenuSurface{
		menu: menu, kind: "tray", id: t.id,
	})
	params := t.spec("setmenu")
	if menu != nil {
		params["menu"] = electronSerializeMenu(menu.items)
	}
	if err := t.electronTrayCall("trayOp", params, nil); err != nil {
		globalApplication.error("electron: tray setMenu: %v", err)
	}
}

// openMenu: tray.popUpContextMenu is macOS/Windows only. The linux native
// tray logs the same gap — with a context menu attached, Electron's linux
// tray already opens it natively on click.
func (t *electronSystemTray) openMenu() {}

func (t *electronSystemTray) bounds() (*Rect, error) {
	var b struct {
		X      int `json:"x"`
		Y      int `json:"y"`
		Width  int `json:"width"`
		Height int `json:"height"`
	}
	if err := t.electronTrayCall("trayOp", t.spec("bounds"), &b); err != nil {
		return nil, err
	}
	return &Rect{
		X:      b.X,
		Y:      b.Y,
		Width:  b.Width,
		Height: b.Height,
	}, nil
}

func (t *electronSystemTray) getScreen() (*Screen, error) {
	// the screen containing the tray icon, falling back to the primary
	if globalApplication != nil {
		if b, err := t.bounds(); err == nil {
			for _, s := range globalApplication.Screen.GetAll() {
				if b.X >= s.Bounds.X && b.X < s.Bounds.X+s.Bounds.Width &&
					b.Y >= s.Bounds.Y && b.Y < s.Bounds.Y+s.Bounds.Height {
					return s, nil
				}
			}
			return globalApplication.Screen.GetPrimary(), nil
		}
	}
	return nil, fmt.Errorf("electron: tray screen unavailable")
}

// positionWindow places an attached window above the tray icon
// (right-aligned), mirroring the linux native tray's placement.
func (t *electronSystemTray) positionWindow(window Window, offset int) error {
	bounds, err := t.bounds()
	if err != nil {
		return err
	}
	w, h := window.Size()
	x := bounds.X + bounds.Width - w
	y := bounds.Y - h - offset
	if y < 0 {
		y = bounds.Y + bounds.Height + offset
	}
	window.SetPosition(x, y)
	return nil
}

// Show/Hide: Electron's Tray has no visibility toggle — destroy on hide
// and rebuild on show from the cached parent state.
func (t *electronSystemTray) Show() {
	if !t.hidden {
		return
	}
	t.hidden = false
	t.create()
}

func (t *electronSystemTray) Hide() {
	if t.hidden {
		return
	}
	t.hidden = true
	if err := t.electronTrayCall("trayOp", t.spec("destroy"), nil); err != nil {
		globalApplication.error("electron: tray hide: %v", err)
	}
}

func (t *electronSystemTray) destroy() {
	if t.hidden {
		return
	}
	if err := t.electronTrayCall("trayOp", t.spec("destroy"), nil); err != nil {
		globalApplication.error("electron: tray destroy: %v", err)
	}
	electronBackend.mu.Lock()
	delete(electronBackend.trays, t.id)
	electronBackend.mu.Unlock()
}
