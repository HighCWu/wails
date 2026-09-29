package application

import (
	"fmt"
	"os"
	"strings"
)

// WebviewBackend selects which engine renders web content inside a window.
type WebviewBackend string

const (
	// WebviewBackendAuto lets Wails pick the engine: the system webview is
	// preferred; the electron backend is used when the system webview is
	// unavailable, or when an Electron runtime is explicitly configured
	// (WAILS_ELECTRON_DIR), signalling intent to run on Electron.
	WebviewBackendAuto WebviewBackend = "auto"

	// WebviewBackendSystem always uses the platform system webview
	// (WebKitGTK on Linux, WebView2 on Windows, WKWebView on macOS).
	WebviewBackendSystem WebviewBackend = "system"

	// WebviewBackendElectron always uses the bundled Electron runtime.
	// Startup fails with a descriptive error when the runtime is not
	// usable — there is no silent fallback, so testing against Electron can
	// never silently test the system webview instead.
	WebviewBackendElectron WebviewBackend = "electron"
)

// EnvWebviewBackend is the environment variable that overrides the
// application.Options.WebviewBackend setting at runtime.
// Valid values: auto, system, electron.
const EnvWebviewBackend = "WAILS_WEBVIEW_BACKEND"

// electronRuntimeProbe reports whether a usable Electron runtime is
// present. It is installed by the electron glue (webview_electron.go) and
// deliberately kept indirect so this file has no dependency on the
// internal/electron package and stays unit-testable on any platform.
var electronRuntimeProbe func() error

// systemWebviewAvailable reports whether the platform system webview can
// be used on this machine. The default returns true (the system webview is
// linked into the binary on every supported platform); platforms that can
// fail at runtime — Windows probing for a WebView2 runtime — override it.
var systemWebviewAvailable = func() bool { return true }

// electronRuntimeConfigured reports whether the user explicitly pointed the
// application at an Electron runtime (via WAILS_ELECTRON_DIR). Presence of
// the variable is treated as intent to prefer Electron under auto.
func electronRuntimeConfigured() bool {
	return strings.TrimSpace(os.Getenv("WAILS_ELECTRON_DIR")) != ""
}

// ParseWebviewBackend converts a configured value into a WebviewBackend.
// Empty input resolves to WebviewBackendAuto.
func ParseWebviewBackend(value string) (WebviewBackend, error) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "":
		return WebviewBackendAuto, nil
	case string(WebviewBackendAuto):
		return WebviewBackendAuto, nil
	case string(WebviewBackendSystem):
		return WebviewBackendSystem, nil
	case string(WebviewBackendElectron):
		return WebviewBackendElectron, nil
	default:
		return "", fmt.Errorf(
			"invalid webview backend %q: must be one of %q, %q or %q",
			value, WebviewBackendAuto, WebviewBackendSystem, WebviewBackendElectron)
	}
}

// resolveWebviewBackend computes the concrete engine for this run.
//
// Precedence: the WAILS_WEBVIEW_BACKEND environment variable overrides
// Options.WebviewBackend; an empty/unset value means auto. Resolution must
// happen before any window is created — engines cannot be swapped after
// native window initialisation.
func resolveWebviewBackend(options Options) (WebviewBackend, error) {
	value := os.Getenv(EnvWebviewBackend)
	if strings.TrimSpace(value) == "" {
		value = string(options.WebviewBackend)
	}
	backend, err := ParseWebviewBackend(value)
	if err != nil {
		return WebviewBackendAuto, err
	}
	if backend == WebviewBackendAuto {
		backend = WebviewBackendSystem
		if electronRuntimeConfigured() || !systemWebviewAvailable() {
			backend = WebviewBackendElectron
		}
	}
	if backend == WebviewBackendElectron && electronRuntimeProbe != nil {
		if err := electronRuntimeProbe(); err != nil {
			return WebviewBackendAuto, err
		}
	}
	return backend, nil
}
