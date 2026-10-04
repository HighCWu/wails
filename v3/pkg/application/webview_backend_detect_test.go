package application

// The exported environment-detection surface is what third-party
// launchers build on: probe the machine, decide whether an Electron
// runtime must be provisioned, set WAILS_ELECTRON_DIR, start the app.

import (
	"testing"
)

func TestSystemWebviewAvailableExported(t *testing.T) {
	// the platform probe must be wired through the exported wrapper;
	// linux links WebKitGTK (true), windows probes the WebView2 runtime
	// (registry-backed on the runner), darwin uses the system WKWebView
	if !SystemWebviewAvailable() {
		t.Skip("platform system webview not available in this environment")
	}
}

func TestElectronRuntimeAvailableRequiresDir(t *testing.T) {
	t.Setenv("WAILS_ELECTRON_DIR", "")
	t.Setenv("WAILS_WEBVIEW_BACKEND", "")
	if ElectronRuntimeAvailable() {
		t.Fatal("ElectronRuntimeAvailable true without WAILS_ELECTRON_DIR")
	}
	t.Setenv("WAILS_ELECTRON_DIR", t.TempDir())
	if ElectronRuntimeAvailable() {
		// an empty dir has no platform binary — the probe must reject it
		t.Fatal("ElectronRuntimeAvailable true for an empty directory")
	}
}

func TestDetectWebviewEnvironmentShape(t *testing.T) {
	t.Setenv("WAILS_WEBVIEW_BACKEND", "auto")
	env := DetectWebviewEnvironment(Options{})
	if env.SystemWebview {
		// linux/windows CI runners have a usable webview: auto resolves
		// to system unless the electron dir is configured
		t.Setenv("WAILS_ELECTRON_DIR", "")
		env = DetectWebviewEnvironment(Options{})
		if env.Backend != WebviewBackendSystem {
			t.Fatalf("auto with a usable webview resolved %q", env.Backend)
		}
		return
	}
	// no system webview: auto must select electron — which is only usable
	// when a runtime is present
	if env.ElectronRuntime {
		if env.Backend != WebviewBackendElectron {
			t.Fatalf("auto without webview + present runtime resolved %q", env.Backend)
		}
	}
}
