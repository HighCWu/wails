//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios

// Package cef provides a minimal, dependency-free cgo binding for the
// Chromium Embedded Framework (CEF) C API, plus the Wails integration
// pieces: process bootstrap, http://wails.localhost asset serving and the
// window.wails.invoke IPC bridge.
//
// libcef is loaded at runtime via dlopen from a CEF runtime directory
// (never linked at build time), so binaries built with this package run
// unchanged on machines without any CEF files. The vendored headers under
// include/ are the official CEF headers matching the runtime this binding
// was developed against (see include/LICENSE.txt); the C API is stable
// across CEF versions but new struct fields may require a header refresh.
//
// Native desktop backends share this package; UI toolkit dependencies remain
// in the application layer so helper processes can bootstrap independently.
package cef

import "sync/atomic"

// CEF runtime layout (merged Release/ + Resources/ from a cef-builds
// "minimal" distribution):
//
//	<dir>/libcef.so
//	<dir>/icudtl.dat
//	<dir>/v8_context_snapshot.bin
//	<dir>/*.pak
//	<dir>/locales/
//
// and, for subprocess execution, the host executable itself (CEF re-execs
// the current binary with --type= switches for renderer/gpu/utility
// processes).

// State shared across the package. All fields are set by Initialize (or
// ExecuteProcess for subprocesses) and are immutable afterwards, except
// the browser table which is guarded by its own mutex.
type State struct {
	// Dir is the resolved CEF runtime directory.
	Dir string

	// MainThreadExecJS et al are installed by the platform glue before
	// Initialize and are called from CEF threads. They marshal work onto
	// the UI/main thread where required.
	DispatchMain func(fn func())

	// PumpHostLoop drains host UI events while waiting for browsers to close.
	PumpHostLoop func()

	// OnWindowMessage delivers a window.wails.invoke message from the
	// renderer (windowID identifies the wails window, origin is the frame
	// URL the message came from).
	OnWindowMessage func(windowID uint, message string, origin string)

	// OnWindowLoadEnd reports that the main frame finished loading.
	OnWindowLoadEnd func(windowID uint)

	// OnWindowLoadStart reports that the main frame started loading.
	OnWindowLoadStart func(windowID uint)

	// OnTitleChange reports a document title change.
	OnTitleChange func(windowID uint, title string)

	// OnBrowserClosing handles the native top-level close after CEF approves it.
	// Return true when the host will destroy its window itself.
	OnBrowserClosing func(windowID uint) bool

	// OnBrowserClosed reports that a browser was destroyed (window closed).
	OnBrowserClosed func(windowID uint)

	// AssetRequest submits an asset request into the wails asset server.
	// It must not block the calling CEF thread; the wails consumer runs
	// the actual processing on its own goroutine. The ResponseWriter is
	// written from that goroutine until Finish.
	AssetRequest func(req *AssetRequest)

	// WindowName resolves a wails window name from a wails window id.
	WindowName func(windowID uint) string

	// OnKeyEvent reports a raw key press from the embedded browser.
	// nativeKeyCode is the platform key code (X hardware keycode on Linux),
	// modifiers is a bitmask of EventFlag* constants. Returns true when
	// the host consumed the event (it must not reach the renderer).
	OnKeyEvent func(windowID uint, nativeKeyCode uint32, modifiers uint32) bool

	// OnFileDrop delivers OS-resolved paths for a completed main-frame drop.
	OnFileDrop func(windowID uint, files []string, x, y int)

	// OnMediaPermission decides a getUserMedia request; mirrors the
	// system webview permission handling.
	OnMediaPermission func(windowID uint, needAudio, needVideo bool) bool
}

// cef_key_event_t modifier flags (cef_types.h).
const (
	EventFlagNone              = 0
	EventFlagShiftDown         = 1 << 1
	EventFlagControlDown       = 1 << 2
	EventFlagAltDown           = 1 << 3
	EventFlagCapsLockOn        = 1 << 0
	EventFlagLeftMouseButton   = 1 << 4
	EventFlagMiddleMouseButton = 1 << 5
	EventFlagRightMouseButton  = 1 << 6
	EventFlagNumLockOn         = 1 << 8
)

// AssetRequest carries everything the glue needs to feed one CEF scheme
// request into the wails asset server.
type AssetRequest struct {
	// Request wraps the CEF request object for the host asset-server adapter.
	Request *assetRequest
	// WindowID is the wails window id the request belongs to (0 unknown).
	WindowID uint
}

var state atomic.Pointer[State]

// Current returns the process-wide CEF state, or nil before Initialize.
func Current() *State { return state.Load() }
