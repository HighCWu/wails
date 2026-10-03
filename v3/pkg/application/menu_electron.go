package application

// Electron-backend menu impls: menus rendered through the serialization
// pipeline (window menubar, application menu, tray menus) have no live
// native widgets, so programmatic state changes must re-serialize and
// re-push to stay visible. Installing these impls at attach time routes
// the existing Menu.Update() / MenuItem mutator dispatch through that
// sync — matching the native backends, where an opened menu always
// reflects current state. Context menus are not attached (they
// re-serialize on every open anyway).

import "fmt"

// electronMenuSurface is one menu attached to one electron surface.
type electronMenuSurface struct {
	menu *Menu
	kind string // "menubar" | "app" | "tray"
	id   uint   // window id for menubar, tray id for tray
}

func (s *electronMenuSurface) sync() {
	switch s.kind {
	case "tray":
		params := map[string]any{"id": s.id, "action": "setmenu", "menu": electronSerializeMenu(s.menu.items)}
		if err := electronProcessCall("trayOp", 0, params, nil); err != nil {
			globalApplication.error("electron: tray menu resync: %v", err)
		}
	default: // "menubar" or "app" — both ride the setMenu spec (id 0 = app)
		electronBackend.mu.Lock()
		electronBackend.menus[s.id] = s.menu
		electronBackend.mu.Unlock()
		sent := false
		for _, w := range electronWindowsSnapshot() {
			if err := w.call("setMenu", map[string]any{
				"id":   s.id,
				"menu": electronSerializeMenu(s.menu.items),
			}); err == nil {
				sent = true
			}
			break
		}
		if !sent {
			globalApplication.error("electron: menu resync: no live window")
		}
	}
}

// attachElectronMenuImpls installs the electron impls on the menu and its
// whole item tree, so mutators and Update() re-sync the surface.
func attachElectronMenuImpls(menu *Menu, s *electronMenuSurface) {
	menu.impl = &electronMenuImpl{s: s}
	var walk func(items []*MenuItem)
	walk = func(items []*MenuItem) {
		for _, it := range items {
			it.impl = &electronMenuItemImpl{s: s}
			if it.submenu != nil {
				it.submenu.impl = &electronMenuImpl{s: s}
				walk(it.submenu.items)
			}
		}
	}
	walk(menu.items)
}

// electronMenuImpl implements menuImpl.
type electronMenuImpl struct {
	s *electronMenuSurface
}

func (m *electronMenuImpl) update() { m.s.sync() }

// electronMenuItemImpl implements menuItemImpl. Every mutator re-syncs
// the whole menu: the serialized template is rebuilt from current Go
// state, which is exactly the state the mutator just changed. By the
// time a click's handleClick reaches us the menu has closed, so the
// re-push never interrupts an open popup.
type electronMenuItemImpl struct {
	s *electronMenuSurface
}

func (i *electronMenuItemImpl) setTooltip(string)                  { i.s.sync() }
func (i *electronMenuItemImpl) setLabel(string)                    { i.s.sync() }
func (i *electronMenuItemImpl) setDisabled(bool)                   { i.s.sync() }
func (i *electronMenuItemImpl) setChecked(bool)                    { i.s.sync() }
func (i *electronMenuItemImpl) setAccelerator(*accelerator)        { i.s.sync() }
func (i *electronMenuItemImpl) setHidden(bool)                     { i.s.sync() }
func (i *electronMenuItemImpl) setBitmap([]byte)                   { i.s.sync() }
func (i *electronMenuItemImpl) destroy()                           {}

// electronProcessCall sends a control-plane request through the live
// electron process (window id 0 for non-window ops like tray ops).
func electronProcessCall(method string, windowID uint, params map[string]any, out any) error {
	electronBackend.mu.Lock()
	proc := electronBackend.proc
	electronBackend.mu.Unlock()
	if proc == nil {
		return fmt.Errorf("electron: process not running")
	}
	return proc.Call(method, windowID, params, out)
}
