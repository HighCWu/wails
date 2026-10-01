package electron

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"os"
	"path/filepath"
)

//go:embed assets/main.js
var mainJS []byte

//go:embed assets/main-js.js
var mainJsJS []byte

//go:embed assets/preload.js
var preloadJS []byte

// ExtractBootstrap writes the embedded bootstrap application to the user
// cache directory and returns (mainPath, preloadPath). The directory name
// carries a content hash so stale copies never shadow an updated bootstrap.
// main-js.js is the JS control plane used on windows (main.js routes to it
// there) and lives next to main.js so require('./main-js.js') resolves.
func ExtractBootstrap() (string, string, error) {
	sum := sha256.Sum256(append(append(append([]byte{}, mainJS...), mainJsJS...), preloadJS...))
	dir, err := os.UserCacheDir()
	if err != nil {
		dir = os.TempDir()
	}
	out := filepath.Join(dir, "wails-electron", "bootstrap-"+hex.EncodeToString(sum[:8]))
	if err := os.MkdirAll(out, 0o755); err != nil {
		return "", "", err
	}
	mainPath := filepath.Join(out, "main.js")
	preloadPath := filepath.Join(out, "preload.js")
	if err := os.WriteFile(mainPath, mainJS, 0o644); err != nil {
		return "", "", err
	}
	if err := os.WriteFile(filepath.Join(out, "main-js.js"), mainJsJS, 0o644); err != nil {
		return "", "", err
	}
	if err := os.WriteFile(preloadPath, preloadJS, 0o644); err != nil {
		return "", "", err
	}
	return mainPath, preloadPath, nil
}
