// Off-screen rendering (windowless) support: the Go side of the render
// handler, the per-browser frame store, and the Browser wrappers the host
// uses to composite frames and forward input. See osr-design.md for the
// stage this implements.

//go:build wails_cef

package cef

/*
#include "cef_glue.h"
*/
import "C"

import (
	"unsafe"
)

// PaintElementType values (cef_paint_element_type_t).
const (
	paintElementView  = 0
	paintElementPopup = 1
)

// Key event types (cef_key_event_type_t).
const (
	keyEventRawkeydown = 0
	keyEventKeyup      = 1
	keyEventChar       = 2
)

// Modifier flags (cef_event_flags_t).
const (
	eventFlagShiftDown         = 1 << 1
	eventFlagControlDown       = 1 << 2
	eventFlagAltDown           = 1 << 3
	eventFlagLeftMouseButton   = 1 << 4
	eventFlagMiddleMouseButton = 1 << 5
	eventFlagRightMouseButton  = 1 << 6
)

// Mouse button types (cef_mouse_button_type_t).
const (
	mouseButtonLeft   = 0
	mouseButtonMiddle = 1
	mouseButtonRight  = 2
)

// SetFrameCallback registers cb to run whenever a new OSR frame is
// composited. The callback executes on the CEF UI thread (the host main
// thread under the wails pump) and must only trigger a redraw; read the
// pixels with FrameSnapshot inside the draw handler.
func (b *Browser) SetFrameCallback(cb func()) {
	if b == nil || b.client == nil {
		return
	}
	b.client.renderMu.Lock()
	b.client.onFrame = cb
	b.client.renderMu.Unlock()
}

// FrameSnapshot copies the latest composited OSR frame (BGRA, premultiplied,
// top-left origin). ok is false until the first paint or when the browser
// is not windowless.
func (b *Browser) FrameSnapshot() (w, h int, buf []byte, ok bool) {
	if b == nil || b.client == nil {
		return 0, 0, nil, false
	}
	b.client.renderMu.Lock()
	defer b.client.renderMu.Unlock()
	if b.client.frameW == 0 || b.client.frameHt == 0 || len(b.client.frameBuf) == 0 {
		return 0, 0, nil, false
	}
	out := make([]byte, len(b.client.frameBuf))
	copy(out, b.client.frameBuf)
	return b.client.frameW, b.client.frameHt, out, true
}

// Windowless reports whether the browser renders off-screen.
func (b *Browser) Windowless() bool {
	return b != nil && b.client != nil && b.client.windowless
}

// Resized tells CEF the OSR view size changed; results in a GetViewRect
// round trip and a repaint at the new size.
func (b *Browser) Resized() {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_was_resized(b.host)
}

// SendMouseMotion forwards a mouse move (or leave when leave is true).
func (b *Browser) SendMouseMotion(x, y int, leave bool) {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_send_mouse_move(b.host, C.int(x), C.int(y), 0, boolToInt(leave))
}

// SendMouseClick forwards a press (up=false) or release (up=true).
// button: 0=left 1=middle 2=right. count is the click count (1, 2, 3...).
func (b *Browser) SendMouseClick(x, y int, button int, up bool, count int, mods uint32) {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_send_mouse_click(b.host, C.int(x), C.int(y),
		C.int(mouseModifiers(button, mods)), C.int(button),
		boolToInt(up), C.int(count))
}

// SendMouseWheel forwards a wheel tick; deltas follow CEF's convention
// (positive deltaY scrolls up, deltaX scrolls right).
func (b *Browser) SendMouseWheel(x, y int, deltaX, deltaY int, mods uint32) {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_send_mouse_wheel(b.host, C.int(x), C.int(y), C.int(mods),
		C.int(deltaX), C.int(deltaY))
}

// SendKeyEvent forwards one translated key event. kind: 0=RAWKEYDOWN
// 1=KEYUP 2=CHAR.
func (b *Browser) SendKeyEvent(kind, mods, windowsKeyCode, nativeKeyCode int, ch, unmodifiedCh uint16, isSystemKey bool) {
	if b == nil || b.host == nil {
		return
	}
	C.wcef_host_send_key_event(b.host, C.int(kind), C.int(mods),
		C.int(windowsKeyCode), C.int(nativeKeyCode), C.ushort(ch),
		C.ushort(unmodifiedCh), boolToInt(isSystemKey), 0)
}

func boolToInt(v bool) C.int {
	if v {
		return 1
	}
	return 0
}

func mouseModifiers(button int, extra uint32) int {
	switch button {
	case mouseButtonLeft:
		return int(eventFlagLeftMouseButton) | int(extra)
	case mouseButtonMiddle:
		return int(eventFlagMiddleMouseButton) | int(extra)
	case mouseButtonRight:
		return int(eventFlagRightMouseButton) | int(extra)
	}
	return int(extra)
}

// ---- Go side of the render handler (exported for the C shims) ----

//export wailsCEFClientGetRenderH
func wailsCEFClientGetRenderH(self *C.struct__cef_client_t) *C.struct__cef_render_handler_t {
	if bc := clientByPtr(unsafe.Pointer(self)); bc != nil {
		return bc.renderH
	}
	return nil
}

//export wailsCEFRenderGetViewRect
func wailsCEFRenderGetViewRect(self *C.struct__cef_render_handler_t,
	browser *C.struct__cef_browser_t, rect *C.cef_rect_t) {
	bc := clientByHandlerPtr(unsafe.Pointer(self))
	if bc == nil || rect == nil {
		return
	}
	w, h := bc.osrSize()
	rect.width = C.int(w)
	rect.height = C.int(h)
}

//export wailsCEFRenderGetScreenInfo
func wailsCEFRenderGetScreenInfo(self *C.struct__cef_render_handler_t,
	browser *C.struct__cef_browser_t, screenInfo *C.cef_screen_info_t) int {
	bc := clientByHandlerPtr(unsafe.Pointer(self))
	if bc == nil || screenInfo == nil {
		return 0
	}
	w, h := bc.osrSize()
	// device_scale_factor 1 keeps buffer pixels equal to view DIPs for the
	// first milestone; a host-provided factor plugs in here later.
	screenInfo.device_scale_factor = 1.0
	screenInfo.rect.x = 0
	screenInfo.rect.y = 0
	screenInfo.rect.width = C.int(w)
	screenInfo.rect.height = C.int(h)
	return 1
}

//export wailsCEFRenderOnPaint
func wailsCEFRenderOnPaint(self *C.struct__cef_render_handler_t,
	browser *C.struct__cef_browser_t, paintType C.cef_paint_element_type_t,
	dirtyRectsCount C.size_t, dirtyRects *C.cef_rect_t,
	buffer unsafe.Pointer, width C.int, height C.int) {
	bc := clientByHandlerPtr(unsafe.Pointer(self))
	if bc == nil || buffer == nil || int(width) <= 0 || int(height) <= 0 {
		return
	}
	// M1 composites only the view surface; popup (select menus) follows.
	if int(paintType) != paintElementView {
		return
	}
	size := int(width) * int(height) * 4
	bc.renderMu.Lock()
	if cap(bc.frameBuf) < size {
		bc.frameBuf = make([]byte, size)
	}
	bc.frameBuf = bc.frameBuf[:size]
	copy(bc.frameBuf, unsafe.Slice((*byte)(buffer), size))
	bc.frameW, bc.frameHt = int(width), int(height)
	bc.frameVer++
	cb := bc.onFrame
	bc.renderMu.Unlock()
	if cb != nil {
		cb()
	}
}

//export wailsCEFRenderOnPopupShow
func wailsCEFRenderOnPopupShow(self *C.struct__cef_render_handler_t,
	browser *C.struct__cef_browser_t, show C.int) {
	// Popup compositing lands with the M3 input milestone; CEF tolerates
	// popups being left invisible.
}

//export wailsCEFRenderOnPopupSize
func wailsCEFRenderOnPopupSize(self *C.struct__cef_render_handler_t,
	browser *C.struct__cef_browser_t, rect *C.cef_rect_t) {
	// See OnPopupShow.
}

// osrSize returns the current OSR view size tracked by the host: the host
// registers it through SetOSRSize so GetViewRect answers with the live
// widget size even before CEF asks through the browser object.
func (bc *browserClient) osrSize() (int, int) {
	bc.renderMu.Lock()
	defer bc.renderMu.Unlock()
	if bc.frameW != 0 && bc.frameHt != 0 {
		return bc.frameW, bc.frameHt
	}
	return bc.osrW, bc.osrH
}

// SetOSRSize records the host widget size used by GetViewRect.
func (b *Browser) SetOSRSize(w, h int) {
	if b == nil || b.client == nil {
		return
	}
	b.client.renderMu.Lock()
	b.client.osrW, b.client.osrH = w, h
	b.client.renderMu.Unlock()
	if b.host != nil {
		C.wcef_host_was_resized(b.host)
	}
}
