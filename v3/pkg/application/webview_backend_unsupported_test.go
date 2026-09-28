//go:build !linux || !wails_cef

package application

import "testing"

// Test the real build configuration, without installing a fake CEF probe.
func TestWebviewBackendUnsupportedBuild(t *testing.T) {
	t.Setenv(EnvWebviewBackend, "")
	t.Setenv("WAILS_CEF_DIR", t.TempDir())
	if probeCEFRuntime != nil {
		t.Fatal("unsupported build unexpectedly registered a CEF runtime")
	}
	if _, err := resolveWebviewBackend(Options{WebviewBackend: WebviewBackendCEF}); err == nil {
		t.Fatal("forcing CEF must fail in an unsupported build")
	}
	for _, requested := range []WebviewBackend{WebviewBackendAuto, WebviewBackendSystem} {
		got, err := resolveWebviewBackend(Options{WebviewBackend: requested})
		if err != nil || got != WebviewBackendSystem {
			t.Fatalf("%s: got %s, %v; want system", requested, got, err)
		}
	}
}
