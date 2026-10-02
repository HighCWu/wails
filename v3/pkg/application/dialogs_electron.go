package application

// Electron-backend dialog implementations. Everything routes through the
// stdio control protocol to the addon, which calls Electron's dialog
// module (showOpenDialog/showSaveDialog/showMessageBox) and returns its
// promise — the control client is promise-aware, so results flow back as
// ordinary responses. Params are already in Electron's option shape;
// the addon copies them onto the options object.
//
// show() semantics follow the per-GOOS native impls: single-selection
// callers read one value from the channel, multi-selection callers range
// it — so the channel is always closed after the paths are delivered.

import (
	"strings"

	"github.com/wailsapp/wails/v3/internal/electron"
)

// ErrElectronNotRunning reports a control-plane request made before the
// Electron process was started (or after it exited).
var ErrElectronNotRunning = errElectronNotRunning{}

type errElectronNotRunning struct{}

func (errElectronNotRunning) Error() string { return "electron process is not running" }

func electronDialogProc() (*electron.Process, error) {
	electronBackend.mu.Lock()
	proc := electronBackend.proc
	electronBackend.mu.Unlock()
	if proc == nil {
		return nil, ErrElectronNotRunning
	}
	return proc, nil
}

func electronDialogWindowID(w Window) uint {
	if w == nil {
		return 0
	}
	return w.ID()
}

func electronFilters(filters []FileFilter) []map[string]any {
	out := make([]map[string]any, 0, len(filters))
	for _, f := range filters {
		exts := []string{}
		for _, ext := range strings.Split(f.Pattern, ";") {
			ext = strings.TrimSpace(ext)
			ext = strings.TrimPrefix(ext, "*.")
			if ext != "" {
				exts = append(exts, ext)
			}
		}
		if len(exts) > 0 {
			out = append(out, map[string]any{"name": f.DisplayName, "extensions": exts})
		}
	}
	return out
}

type electronOpenFileDialog struct {
	d *OpenFileDialogStruct
}

func (e *electronOpenFileDialog) show() (chan string, error) {
	d := e.d
	ch := make(chan string, 8)
	go func() {
		defer close(ch)
		proc, err := electronDialogProc()
		if err != nil {
			return
		}
		var props []string
		if d.canChooseFiles {
			props = append(props, "openFile")
		}
		if d.canChooseDirectories {
			props = append(props, "openDirectory", "createDirectory")
		}
		if d.allowsMultipleSelection {
			props = append(props, "multiSelections")
		}
		if d.showHiddenFiles {
			props = append(props, "showHiddenFiles")
		}
		params := map[string]any{
			"title":     d.title,
			"button":    d.buttonText,
			"filters":   electronFilters(d.filters),
			"windowID":  electronDialogWindowID(d.window),
			"properties": props,
		}
		if d.directory != "" {
			params["defaultPath"] = d.directory
		}
		var res struct {
			Canceled  bool     `json:"canceled"`
			FilePaths []string `json:"filePaths"`
		}
		if err := proc.Call("showOpenDialog", electronDialogWindowID(d.window), params, &res); err != nil {
			return
		}
		for _, p := range res.FilePaths {
			ch <- p
		}
	}()
	return ch, nil
}

type electronSaveFileDialog struct {
	d *SaveFileDialogStruct
}

func (e *electronSaveFileDialog) show() (chan string, error) {
	d := e.d
	ch := make(chan string, 1)
	go func() {
		defer close(ch)
		proc, err := electronDialogProc()
		if err != nil {
			return
		}
		params := map[string]any{
			"title":    d.title,
			"button":   d.buttonText,
			"filters":  electronFilters(d.filters),
			"windowID": electronDialogWindowID(d.window),
		}
		switch {
		case d.directory != "" && d.filename != "":
			params["defaultPath"] = d.directory + "/" + d.filename
		case d.directory != "":
			params["defaultPath"] = d.directory
		case d.filename != "":
			params["defaultPath"] = d.filename
		}
		var res struct {
			Canceled bool   `json:"canceled"`
			FilePath string `json:"filePath"`
		}
		if err := proc.Call("showSaveDialog", electronDialogWindowID(d.window), params, &res); err != nil {
			return
		}
		if res.FilePath != "" {
			ch <- res.FilePath
		}
	}()
	return ch, nil
}

type electronMessageDialog struct {
	d *MessageDialog
}

func (e *electronMessageDialog) show() {
	go func() {
		proc, err := electronDialogProc()
		if err != nil {
			return
		}
		d := e.d
		dialogType := "info"
		switch d.DialogType {
		case QuestionDialogType:
			dialogType = "question"
		case WarningDialogType:
			dialogType = "warning"
		case ErrorDialogType:
			dialogType = "error"
		}
		buttons := []string{}
		defaultID, cancelID := 0, -1
		for i, b := range d.Buttons {
			buttons = append(buttons, b.Label)
			if b.IsDefault {
				defaultID = i
			}
			if b.IsCancel {
				cancelID = i
			}
		}
		// Electron throws on an empty buttons array
		if len(buttons) == 0 {
			buttons = []string{"OK"}
			defaultID = 0
		}
		params := map[string]any{
			"type":      dialogType,
			"title":     d.Title,
			"message":   d.Message,
			"buttons":   buttons,
			"defaultId": defaultID,
			"windowID":  electronDialogWindowID(d.window),
		}
		if cancelID >= 0 {
			params["cancelId"] = cancelID
		}
		_ = proc.Call("showMessageDialog", electronDialogWindowID(d.window), params, nil)
	}()
}
