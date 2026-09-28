//go:build (linux || windows || darwin) && cgo && wails_cef && !android && !ios

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15400
*/
import "C"

import (
	"fmt"
	"os"
	"path/filepath"
	"sync/atomic"
	"time"
	"unsafe"
)

// Process roles.
const (
	// CEF re-executes the host binary with a --type= switch for
	// renderer/gpu/utility/zygote subprocesses. Anything carrying that
	// switch must run the CEF subprocess loop instead of the wails app.
	subprocessSwitch = "--type="
)

// IsSubprocess reports whether this process was spawned by CEF as a
// subprocess (--type= switch present).
func IsSubprocess() bool {
	for _, arg := range os.Args {
		if len(arg) > len(subprocessSwitch) && arg[:len(subprocessSwitch)] == subprocessSwitch {
			return true
		}
	}
	return false
}

// mainArgs mirrors os.Args into a cef_main_args_t. The returned args and
// the backing C strings must stay alive for the duration of the CEF call.
type mainArgs struct {
	c    C.cef_main_args_t
	ptrs []unsafe.Pointer
}

func newMainArgs() *mainArgs {
	ma := &mainArgs{}

	argv := (**C.char)(C.calloc(C.size_t(len(os.Args)+1), C.size_t(unsafe.Sizeof((*C.char)(nil)))))
	base := unsafe.Pointer(argv)
	for i, arg := range os.Args {
		cs := C.CString(arg)
		ma.ptrs = append(ma.ptrs, unsafe.Pointer(cs))
		*(**C.char)(unsafe.Add(base, unsafe.Sizeof((*C.char)(nil))*uintptr(i))) = cs
	}
	C.wcef_main_args(&ma.c, C.int(len(os.Args)), argv)
	ma.ptrs = append(ma.ptrs, base)
	return ma
}

func (ma *mainArgs) free() {
	for _, p := range ma.ptrs {
		C.free(p)
	}
	ma.ptrs = nil
}

// ExecuteSubprocess runs the CEF subprocess message loop for renderer/gpu/
// utility processes and exits the process with CEF's exit code. It must be
// called as the very first action of the program (before GTK, the wails
// application or any other heavy initialisation) whenever IsSubprocess()
// is true.
func ExecuteSubprocess() error {
	dir, err := RuntimeDir("")
	if err != nil {
		return fmt.Errorf("CEF subprocess cannot start: %w", err)
	}
	if err := loadLibrary(dir); err != nil {
		return fmt.Errorf("CEF subprocess cannot load runtime: %w", err)
	}
	if err := initState(dir); err != nil {
		return err
	}

	ma := newMainArgs()
	defer ma.free()
	app := buildApp()

	code := C.wcef_execute_process(&ma.c, app)
	// >= 0: this was a subprocess and it has finished; exit with CEF's code.
	// < 0: not a subprocess — the caller should never see this because
	// ExecuteSubprocess is only invoked when IsSubprocess() is true.
	exitCode := int(code)
	os.Exit(exitCode)
	return nil
}

// InitializeOptions tunes the browser-process bootstrap.
type InitializeOptions struct {
	// Dir is an explicit CEF runtime directory (overrides WAILS_CEF_DIR
	// and the default probes).
	Dir string

	// CachePath overrides the on-disk cache location. Empty selects
	// <user cache dir>/<executable name>-cef (or in-memory cache when the
	// user cache dir cannot be resolved).
	CachePath string

	// ExtraSwitches are appended to the browser command line (without
	// the leading --), e.g. "disable-gpu-compositing".
	ExtraSwitches []string

	// EnableSandbox opts into the Chromium sandbox when the runtime
	// directory ships a setuid chrome-sandbox helper. Off by default:
	// the Go runtime is not designed for sandboxed processes.
	EnableSandbox bool

	// LogToFile routes CEF's own log to <dir>/debug.log and enables
	// verbose logging. Useful when debugging the runtime itself.
	LogToFile bool
}

var (
	initialized       atomic.Bool
	initializeOptions InitializeOptions
)

// Initialize loads libcef from the resolved runtime directory and starts
// the CEF browser process. Must be called on the main OS thread before
// any browser is created, and before GTK starts pumping events
// (multi-threaded message loop: CEF runs its UI thread, the main thread
// stays with the host toolkit).
func Initialize(opts InitializeOptions) error {
	if initialized.Load() {
		return fmt.Errorf("cef: already initialized")
	}

	dir, err := RuntimeDir(opts.Dir)
	if err != nil {
		return err
	}
	if err := checkRuntimeDir(dir); err != nil {
		return err
	}
	if err := loadLibrary(dir); err != nil {
		return fmt.Errorf("cef: %w", err)
	}
	if err := initState(dir); err != nil {
		return err
	}
	initializeOptions = opts

	settings, free := buildSettings(dir, opts)
	defer free()

	ma := newMainArgs()
	defer ma.free()
	app := buildApp()

	if C.wcef_initialize(&ma.c, settings, app) != 1 {
		return fmt.Errorf("cef: cef_initialize failed (see %s)", logHint(opts))
	}

	// Register the http://wails.localhost scheme handler on the global request context.
	// Documented as callable from any browser-process thread.
	if !registerSchemeFactory() {
		return fmt.Errorf("cef: cef_register_scheme_handler_factory failed")
	}

	initialized.Store(true)
	return nil
}

func logHint(opts InitializeOptions) string {
	if opts.LogToFile {
		return "the CEF log next to the runtime"
	}
	return "WAILS_CEF_LOG_TO_FILE=1 for the CEF log"
}

// Initialized reports whether the browser process CEF state is live.
func Initialized() bool { return initialized.Load() }

// DoMessageLoopWork pumps CEF's message loop once; the host calls it
// from its UI loop (~10ms cadence) when multi_threaded_message_loop is
// disabled.
func DoMessageLoopWork() {
	if !initialized.Load() {
		return
	}
	C.wcef_do_message_loop_work()
}

// shutdownGracePeriod bounds how long CloseAllBrowsers may block when
// tearing the browser process down.
const shutdownGracePeriod = 10 * time.Second

// Shutdown tears down CEF on the main thread after the host UI loop has
// stopped: force-closes any browsers still alive (windows that never
// closed) and waits for their destruction before cef_shutdown, which
// hangs otherwise.
func Shutdown() {
	if !initialized.Swap(false) {
		return
	}
	if remaining := CloseAllBrowsers(shutdownGracePeriod); remaining > 0 {
		pkgLogger().Error("CEF shutdown skipped: live browsers remain", "count", remaining)
		// CEF forbids shutdown before every OnBeforeClose callback. Keep the
		// library loaded until process exit rather than triggering its assertions.
		return
	}
	C.wcef_shutdown()
}

// initState installs the package-level State. Glue hooks are optional in
// subprocesses (only the V8 IPC extension runs there); the browser
// process glue must set them before Initialize.
func initState(dir string) error {
	if state.Load() != nil {
		return nil
	}
	st := &State{}
	if defaultStateHooks != nil {
		*st = *defaultStateHooks
	}
	st.Dir = dir
	state.CompareAndSwap(nil, st)
	return nil
}

// defaultStateHooks is set by the platform glue before Initialize; see
// cef.go for the field meanings.
var defaultStateHooks *State

// SetStateHooks installs the glue callbacks used by the CEF event
// handlers. Must be called before Initialize/ExecuteSubprocess.
func SetStateHooks(hooks *State) { defaultStateHooks = hooks }

// buildSettings fills a cef_settings_t for the browser process. CEF never
// calls methods on the settings struct, so it is a plain calloc'd value;
// the returned cleanup frees it along with every owned string.
func buildSettings(dir string, opts InitializeOptions) (*C.cef_settings_t, func()) {
	s := (*C.cef_settings_t)(C.calloc(1, C.sizeof_cef_settings_t))
	s.size = C.sizeof_cef_settings_t

	setStr := func(field *C.cef_string_t, value string) *cefString {
		cs := newCefString(value)
		*field = cs.c
		return cs
	}

	var owned []*cefString
	exe, _ := os.Executable()

	subprocessPath := os.Getenv("WAILS_CEF_SUBPROCESS_PATH")
	if subprocessPath == "" {
		subprocessPath = exe
	}
	owned = append(owned, setStr(&s.browser_subprocess_path, subprocessPath))
	owned = append(owned, setStr(&s.framework_dir_path, frameworkDir(dir)))
	owned = append(owned, setStr(&s.resources_dir_path, resourcesDir(dir)))
	owned = append(owned, setStr(&s.locales_dir_path, filepath.Join(dir, "locales")))

	cache := opts.CachePath
	if cache == "" {
		if userCache, err := os.UserCacheDir(); err == nil && exe != "" {
			cache = filepath.Join(userCache, filepath.Base(exe)+"-cef")
			_ = os.MkdirAll(cache, 0o755)
		}
	}
	if cache != "" {
		// Layout required by CEF: cache_path must live INSIDE
		// root_cache_path; root_cache_path scopes the process singleton.
		owned = append(owned, setStr(&s.cache_path, filepath.Join(cache, "Cache")))
		owned = append(owned, setStr(&s.root_cache_path, cache))
	}

	if opts.LogToFile {
		owned = append(owned, setStr(&s.log_file, filepath.Join(dir, "debug.log")))
		s.log_severity = C.LOGSEVERITY_INFO
	} else {
		s.log_severity = C.LOGSEVERITY_ERROR
	}

	// MTML stays off on Linux: with multi_threaded_message_loop=1 CEF
	// fails to create the browser's native X window in this embedding
	// (verified against a pure-C host pumping the loop manually). The
	// glue pumps CefDoMessageLoopWork from a GTK timeout instead.
	s.multi_threaded_message_loop = 0
	s.windowless_rendering_enabled = 0
	if !opts.EnableSandbox {
		s.no_sandbox = 1
	}
	// CEF's signal handlers conflict with the Go runtime (sigaltstack /
	// panic machinery); never let it install them.
	s.disable_signal_handlers = 1

	return s, func() {
		for _, cs := range owned {
			cs.Clear()
		}
		C.free(unsafe.Pointer(s))
	}
}
