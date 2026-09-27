//go:build linux && wails_cef

package cef

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// DirEnvVar names the environment variable pointing at the CEF runtime
// directory (the merged Release/+Resources/ layout from a cef-builds
// "minimal" distribution; see package doc).
const DirEnvVar = "WAILS_CEF_DIR"

// runtimeFiles must exist in a usable runtime directory. libcef.so is
// checked separately so error messages can name the actual problem.
var requiredFiles = []string{
	"icudtl.dat",
	"v8_context_snapshot.bin",
}

// RuntimeDir resolves the CEF runtime directory:
//
//  1. $WAILS_CEF_DIR
//  2. <executable dir>/cef
//  3. <working dir>/cef
//
// dirExists short-circuits the search when non-empty, treating it as an
// explicit request (errors mention it instead of silently probing
// elsewhere).
func RuntimeDir(explicit string) (string, error) {
	if explicit != "" {
		return explicit, checkRuntimeDir(explicit)
	}
	if env := os.Getenv(DirEnvVar); env != "" {
		return env, checkRuntimeDir(env)
	}

	exe, err := os.Executable()
	if err == nil {
		candidate := filepath.Join(filepath.Dir(exe), "cef")
		if checkRuntimeDir(candidate) == nil {
			return candidate, nil
		}
	}
	if checkRuntimeDir("cef") == nil {
		abs, err := filepath.Abs("cef")
		if err == nil {
			return abs, nil
		}
		return "cef", nil
	}
	return "", errors.New("no CEF runtime directory found: set " + DirEnvVar + " or place the runtime in ./cef next to the executable")
}

// CheckRuntimeDir validates that dir looks like a usable CEF runtime.
func checkRuntimeDir(dir string) error {
	info, err := os.Stat(dir)
	if err != nil {
		return fmt.Errorf("CEF runtime directory %q: %w", dir, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("CEF runtime path %q is not a directory", dir)
	}
	if _, err := os.Stat(filepath.Join(dir, "libcef.so")); err != nil {
		return fmt.Errorf("CEF runtime directory %q has no libcef.so", dir)
	}
	for _, f := range requiredFiles {
		if _, err := os.Stat(filepath.Join(dir, f)); err != nil {
			return fmt.Errorf("CEF runtime directory %q has no %s", dir, f)
		}
	}
	return nil
}

// Probe reports whether a usable CEF runtime is resolvable. It is
// installed into the wails backend resolver by the platform glue.
func Probe() error {
	dir, err := RuntimeDir("")
	if err != nil {
		return err
	}
	return checkRuntimeDir(dir)
}
