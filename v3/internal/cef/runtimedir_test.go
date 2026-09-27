package cef

import (
	"os"
	"path/filepath"
	"testing"
)

// fakeRuntimeDir creates a directory with the minimal runtime file set.
func fakeRuntimeDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	for _, f := range append([]string{"libcef.so"}, requiredFiles...) {
		if err := os.WriteFile(filepath.Join(dir, f), []byte{0}, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestRuntimeDirEnvOverride(t *testing.T) {
	dir := fakeRuntimeDir(t)
	t.Setenv(DirEnvVar, dir)
	got, err := RuntimeDir("")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != dir {
		t.Errorf("RuntimeDir() = %q, want %q", got, dir)
	}
}

func TestRuntimeDirExplicitWinsOverEnv(t *testing.T) {
	t.Setenv(DirEnvVar, t.TempDir())
	explicit := fakeRuntimeDir(t)
	got, err := RuntimeDir(explicit)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != explicit {
		t.Errorf("RuntimeDir(explicit) = %q, want %q", got, explicit)
	}
}

func TestCheckRuntimeDirRejectsEmpty(t *testing.T) {
	if err := checkRuntimeDir(t.TempDir()); err == nil {
		t.Fatal("expected error for directory without libcef.so")
	}
}

func TestCheckRuntimeDirAcceptsComplete(t *testing.T) {
	if err := checkRuntimeDir(fakeRuntimeDir(t)); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestProbeWithoutRuntime(t *testing.T) {
	t.Setenv(DirEnvVar, t.TempDir())
	if err := Probe(); err == nil {
		t.Fatal("expected probe error when runtime is incomplete")
	}
}
