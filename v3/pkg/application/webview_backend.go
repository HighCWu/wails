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
	// preferred; CEF is used when the system webview is unavailable, or when
	// a CEF runtime directory is explicitly configured (WAILS_CEF_DIR),
	// signalling intent to run on CEF.
	WebviewBackendAuto WebviewBackend = "auto"

	// WebviewBackendSystem always uses the platform system webview
	// (WebKitGTK on Linux, WebView2 on Windows, WKWebView on macOS).
	WebviewBackendSystem WebviewBackend = "system"

	// WebviewBackendCEF always uses the bundled CEF runtime. Startup fails
	// with a descriptive error when the CEF runtime is not usable — there is
	// no silent fallback, so testing against CEF can never silently test the
	// system webview instead.
	WebviewBackendCEF WebviewBackend = "cef"
)

// EnvWebviewBackend is the environment variable that overrides the
// application.Options.WebviewBackend setting at runtime.
// Valid values: auto, system, cef.
const EnvWebviewBackend = "WAILS_WEBVIEW_BACKEND"

// cefRuntimeProbe reports whether a usable CEF runtime is present. It is
// installed by the platform glue (linux GTK3 CEF engine today, Windows
// later) and deliberately kept indirect so this file has no dependency on
// the cef package and stays unit-testable on any platform.
type cefRuntimeProbe func() error

var probeCEFRuntime cefRuntimeProbe

// systemWebviewAvailable reports whether the platform system webview can be
// used on this machine. The default returns true (the system webview is
// linked into the binary on every supported platform); platforms that can
// fail at runtime — Windows probing for a WebView2 runtime — override it.
var systemWebviewAvailable = func() bool { return true }

// cefRuntimeConfigured reports whether the user explicitly pointed the
// application at a CEF runtime directory (via WAILS_CEF_DIR). Presence of
// the variable is treated as intent to prefer CEF under auto.
func cefRuntimeConfigured() bool {
	return strings.TrimSpace(os.Getenv("WAILS_CEF_DIR")) != ""
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
	case string(WebviewBackendCEF):
		return WebviewBackendCEF, nil
	default:
		return "", fmt.Errorf(
			"invalid webview backend %q: must be one of %q, %q or %q",
			value, WebviewBackendAuto, WebviewBackendSystem, WebviewBackendCEF)
	}
}

// resolveWebviewBackend computes the concrete engine for this run.
//
// Precedence: the WAILS_WEBVIEW_BACKEND environment variable overrides
// Options.WebviewBackend; an empty/unset value means auto. Resolution must
// happen before any window is created — engines cannot be swapped after
// native window initialisation.
func resolveWebviewBackend(options Options) (WebviewBackend, error) {
	requested := string(options.WebviewBackend)
	if env := os.Getenv(EnvWebviewBackend); strings.TrimSpace(env) != "" {
		requested = env
	}

	want, err := ParseWebviewBackend(requested)
	if err != nil {
		return "", err
	}

	switch want {
	case WebviewBackendSystem:
		return WebviewBackendSystem, nil

	case WebviewBackendCEF:
		if probeCEFRuntime == nil {
			return "", fmt.Errorf("webview backend \"cef\" requested but CEF support is not compiled into this build")
		}
		if err := probeCEFRuntime(); err != nil {
			return "", fmt.Errorf("webview backend \"cef\" requested but the CEF runtime is not usable: %w", err)
		}
		return WebviewBackendCEF, nil

	case WebviewBackendAuto:
		// An explicitly configured CEF directory signals intent to run on
		// CEF; honour it whenever the runtime is actually usable.
		if cefRuntimeConfigured() {
			if probeCEFRuntime == nil || probeCEFRuntime() != nil {
				if systemWebviewAvailable() {
					return WebviewBackendSystem, nil
				}
				return "", fmt.Errorf("no usable webview backend: CEF runtime not usable in %q and the system webview is unavailable", os.Getenv("WAILS_CEF_DIR"))
			}
			return WebviewBackendCEF, nil
		}
		if systemWebviewAvailable() {
			return WebviewBackendSystem, nil
		}
		if probeCEFRuntime != nil && probeCEFRuntime() == nil {
			return WebviewBackendCEF, nil
		}
		return "", fmt.Errorf("no usable webview backend: the system webview is unavailable and no usable CEF runtime was found")
	}

	return "", fmt.Errorf("unhandled webview backend %q", want)
}
