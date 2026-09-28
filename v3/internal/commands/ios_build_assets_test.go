package commands

import (
	"io/fs"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/require"
	"github.com/wailsapp/wails/v3/internal/gosod"
)

// Source templates must not become standalone main packages during ./... builds,
// but extraction must still produce the same Go files in generated projects.
func TestIOSGoTemplatesExtract(t *testing.T) {
	assets, err := fs.Sub(buildAssets, "build_assets/ios")
	require.NoError(t, err)
	output := t.TempDir()
	require.NoError(t, gosod.New(assets).Extract(output, BuildConfig{}))
	for _, name := range []string{"app_options_default.go", "app_options_ios.go", "main_ios.go"} {
		expected, err := fs.ReadFile(assets, name+".tmpl")
		require.NoError(t, err)
		actual, err := os.ReadFile(filepath.Join(output, name))
		require.NoError(t, err)
		require.Equal(t, string(expected), string(actual))
		_, err = os.Stat(filepath.Join(output, name+".tmpl"))
		require.True(t, os.IsNotExist(err))
	}
}
