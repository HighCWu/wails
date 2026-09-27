package application

import (
	"strings"
	"testing"
)

func installCEFRuntimeProbeForTest(t *testing.T, probe cefRuntimeProbe) {
	t.Helper()
	old := probeCEFRuntime
	probeCEFRuntime = probe
	t.Cleanup(func() { probeCEFRuntime = old })
}

func installSystemWebviewAvailableForTest(t *testing.T, available func() bool) {
	t.Helper()
	old := systemWebviewAvailable
	systemWebviewAvailable = available
	t.Cleanup(func() { systemWebviewAvailable = old })
}

func TestParseWebviewBackend(t *testing.T) {
	for _, tc := range []struct {
		in      string
		want    WebviewBackend
		wantErr bool
	}{
		{"", WebviewBackendAuto, false},
		{"auto", WebviewBackendAuto, false},
		{"AUTO", WebviewBackendAuto, false},
		{" system ", WebviewBackendSystem, false},
		{"cef", WebviewBackendCEF, false},
		{"CeF", WebviewBackendCEF, false},
		{"chromium", "", true},
	} {
		got, err := ParseWebviewBackend(tc.in)
		if tc.wantErr {
			if err == nil {
				t.Errorf("ParseWebviewBackend(%q): expected error, got %q", tc.in, got)
			}
			continue
		}
		if err != nil {
			t.Errorf("ParseWebviewBackend(%q): unexpected error: %v", tc.in, err)
			continue
		}
		if got != tc.want {
			t.Errorf("ParseWebviewBackend(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestResolveWebviewBackendDefaultsToSystem(t *testing.T) {
	for _, requested := range []string{"", "auto", "system"} {
		got, err := resolveWebviewBackend(Options{WebviewBackend: WebviewBackend(requested)})
		if err != nil {
			t.Fatalf("requested %q: unexpected error: %v", requested, err)
		}
		if got != WebviewBackendSystem {
			t.Errorf("requested %q: got %q, want system (no CEF runtime present)", requested, got)
		}
	}
}

func TestResolveWebviewBackendForcedCEF(t *testing.T) {
	// No CEF glue compiled in: forcing cef must fail loudly.
	_, err := resolveWebviewBackend(Options{WebviewBackend: WebviewBackendCEF})
	if err == nil {
		t.Fatal("expected error when cef is forced but no runtime is usable")
	}
	if !strings.Contains(err.Error(), "cef") {
		t.Errorf("error should mention cef, got: %v", err)
	}
}

func TestResolveWebviewBackendForcedCEFUsable(t *testing.T) {
	installCEFRuntimeProbeForTest(t, func() error { return nil })

	got, err := resolveWebviewBackend(Options{WebviewBackend: WebviewBackendCEF})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != WebviewBackendCEF {
		t.Errorf("got %q, want cef", got)
	}
}

func TestResolveWebviewBackendEnvOverridesOptions(t *testing.T) {
	installCEFRuntimeProbeForTest(t, func() error { return nil })
	t.Setenv(EnvWebviewBackend, "system")

	// Options say cef, env says system → system wins.
	got, err := resolveWebviewBackend(Options{WebviewBackend: WebviewBackendCEF})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != WebviewBackendSystem {
		t.Errorf("got %q, want system (env override)", got)
	}
}

func TestResolveWebviewBackendEnvInvalid(t *testing.T) {
	t.Setenv(EnvWebviewBackend, "chromium")

	_, err := resolveWebviewBackend(Options{WebviewBackend: WebviewBackendSystem})
	if err == nil {
		t.Fatal("expected error for invalid env value")
	}
}

func TestResolveWebviewBackendAutoPrefersCEFWhenRuntimeDirConfigured(t *testing.T) {
	installCEFRuntimeProbeForTest(t, func() error { return nil })
	t.Setenv("WAILS_CEF_DIR", "/opt/cef")

	got, err := resolveWebviewBackend(Options{})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != WebviewBackendCEF {
		t.Errorf("got %q, want cef (WAILS_CEF_DIR signals intent)", got)
	}
}

func TestResolveWebviewBackendAutoFallsBackWhenRuntimeDirCEFUnusable(t *testing.T) {
	t.Setenv("WAILS_CEF_DIR", "/opt/cef")

	got, err := resolveWebviewBackend(Options{})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != WebviewBackendSystem {
		t.Errorf("got %q, want system fallback when CEF runtime unusable", got)
	}
}

func TestResolveWebviewBackendAutoFallsBackToCEFWhenSystemUnavailable(t *testing.T) {
	installCEFRuntimeProbeForTest(t, func() error { return nil })
	installSystemWebviewAvailableForTest(t, func() bool { return false })

	got, err := resolveWebviewBackend(Options{})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != WebviewBackendCEF {
		t.Errorf("got %q, want cef fallback", got)
	}
}

func TestResolveWebviewBackendNothingUsable(t *testing.T) {
	installSystemWebviewAvailableForTest(t, func() bool { return false })

	_, err := resolveWebviewBackend(Options{})
	if err == nil {
		t.Fatal("expected error when no backend is usable")
	}
}

func TestNewApplicationRecordsBackendError(t *testing.T) {
	t.Setenv(EnvWebviewBackend, "cef")

	oldGlobal := globalApplication
	globalApplication = nil
	t.Cleanup(func() { globalApplication = oldGlobal })

	app := newApplication(Options{Name: "Test"})
	if app.webviewBackendError == nil {
		t.Fatal("expected webviewBackendError to be set when cef is forced without a runtime")
	}
	if app.webviewBackend != WebviewBackendSystem {
		t.Errorf("placeholder backend = %q, want system", app.webviewBackend)
	}
}
