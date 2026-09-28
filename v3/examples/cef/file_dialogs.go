//go:build linux || windows || (darwin && !ios)

package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/wailsapp/wails/v3/pkg/application"
)

// FileDialog uses only Wails' public API, like examples/dialogs. All selectable
// files belong to the test directory supplied by the isolated desktop runner.
func (*ProbeService) FileDialog(name string) {
	root := os.Getenv("CEF_SMOKE_FILE_DIR")
	if root == "" {
		panic("CEF_SMOKE_FILE_DIR is required")
	}
	app := application.Get()
	result := struct {
		Case  string   `json:"case"`
		Paths []string `json:"paths"`
		Error string   `json:"error"`
	}{Case: name, Paths: []string{}}
	var err error
	fmt.Printf("CEF_SMOKE_FILE_OPEN %s\n", name)
	switch name {
	case "save", "save-attached", "cancel-save", "overwrite":
		filename := "saved.txt"
		if name == "save-attached" {
			// Distinct name so the assertion proves the attached dialog
			// ran, not the plain save case.
			filename = "attached.txt"
		}
		if name == "overwrite" {
			// Reuses the fixture's existing.txt so the native
			// replacement confirmation actually appears.
			filename = "existing.txt"
		}
		d := app.Dialog.SaveFile().
			SetMessage("CEF file dialog").
			SetDirectory(root).
			SetFilename(filename).
			AddFilter("Text files", "*.txt")
		if name == "save-attached" {
			w, _ := app.Window.GetByName("main")
			d.AttachToWindow(w)
		}
		var path string
		path, err = d.PromptForSingleSelection()
		if path != "" {
			result.Paths = append(result.Paths, path)
		}
	case "open", "open-attached", "cancel-open", "filter", "multiple", "directory":
		d := app.Dialog.OpenFile().
			SetTitle("CEF file dialog").
			SetDirectory(root).
			CanChooseFiles(true)
		if name == "open-attached" {
			w, _ := app.Window.GetByName("main")
			d.AttachToWindow(w)
		}
		if name == "filter" {
			d.AddFilter("Text files", "*.txt")
		}
		if name == "directory" {
			d.CanChooseDirectories(true).CanChooseFiles(false)
		}
		if name == "multiple" {
			d.SetDirectory(filepath.Join(root, "multiple"))
			result.Paths, err = d.PromptForMultipleSelection()
		} else {
			var path string
			path, err = d.PromptForSingleSelection()
			if path != "" {
				result.Paths = append(result.Paths, path)
			}
		}
	default:
		err = fmt.Errorf("unknown file dialog case %q", name)
	}
	if err != nil {
		result.Error = err.Error()
	}
	if result.Paths == nil {
		result.Paths = []string{}
	}
	data, _ := json.Marshal(result)
	fmt.Printf("CEF_SMOKE_FILE_RESULT %s\n", strings.TrimSpace(string(data)))
}
