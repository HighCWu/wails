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

// SystemWebviewAvailable reports whether the platform system webview can
// render on this machine: WebKitGTK is linked in on Linux and WKWebView is
// provided by macOS, so those always report true; on Windows it probes for
// an installed WebView2 runtime (the check that drives the auto backend
// decision — Win10 LTSC and Server SKUs frequently ship without one).
//
// Safe to call before application.New — third-party launchers use it to
// decide whether an Electron runtime must be provisioned first.
func SystemWebviewAvailable() bool {
	return systemWebviewAvailable()
}

// ElectronRuntimeAvailable reports whether a usable Electron runtime is
// present right now: WAILS_ELECTRON_DIR points at a distribution
// containing the platform binary. Pair it with SystemWebviewAvailable in
// launcher logic — when both are false the application cannot render.
func ElectronRuntimeAvailable() bool {
	if electronRuntimeProbe == nil {
		return false
	}
	return electronRuntimeProbe() == nil
}

// WebviewEnvironment is the result of probing the machine for webview
// backend selection.
type WebviewEnvironment struct {
	// Backend is the engine the current configuration resolves to (the
	// same decision application.New makes, without starting the app).
	Backend WebviewBackend

	// SystemWebview reports whether the platform system webview can render
	// (false on Windows machines without the WebView2 runtime).
	SystemWebview bool

	// ElectronRuntime reports whether a usable Electron runtime is present.
	ElectronRuntime bool
}

// DetectWebviewEnvironment probes the machine and returns the backend the
// current configuration (Options.WebviewBackend + WAILS_WEBVIEW_BACKEND)
// resolves to, plus the availability facts behind the decision. Launchers
// that download an Electron runtime on demand call this first, provision
// when needed, and re-run after setting WAILS_ELECTRON_DIR.
func DetectWebviewEnvironment(options Options) WebviewEnvironment {
	env := WebviewEnvironment{
		SystemWebview:   systemWebviewAvailable(),
		ElectronRuntime: electronRuntimeConfigured() && electronRuntimeProbe != nil && electronRuntimeProbe() == nil,
	}
	backend, err := resolveWebviewBackend(options)
	if err != nil {
		// an unusable configuration is reported as-is; the error itself
		// surfaces at application.New/Run
		env.Backend = WebviewBackendAuto
		return env
	}
	env.Backend = backend
	return env
}

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
