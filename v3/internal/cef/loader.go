//go:build linux && wails_cef

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15400
*/
import "C"

import (
	"fmt"
	"sync"
	"sync/atomic"
	"unsafe"
)

// loadLibrary dlopens libcef.so from the given runtime directory and
// resolves the C-side symbol table (see cef_capi.c).
func loadLibrary(dir string) error {
	cpath := C.CString(dir + "/libcef.so")
	defer C.free(unsafe.Pointer(cpath))
	if C.wcef_load(cpath) != 1 {
		return fmt.Errorf("%s", C.GoString(C.wcef_load_error()))
	}
	loaded.Store(true)
	return nil
}

var loaded atomic.Bool

// Loaded reports whether a CEF runtime has been dlopen'd in this process.
func Loaded() bool { return loaded.Load() }

// ---------------------------------------------------------------------------
// Object registry and refcounting.
//
// Every CEF struct this package hands to CEF embeds
// cef_base_ref_counted_t as its first member; the callbacks in callbacks.go
// keep a per-object refcount in Go and free the C allocation when it drops
// to zero. Go-side state is looked up from the struct pointer.
// ---------------------------------------------------------------------------

type cefObject struct {
	refs     atomic.Int32
	anchored bool
	onFree   func(self unsafe.Pointer)
}

var objects sync.Map // unsafe.Pointer(C struct) -> *cefObject

// registerObject registers a struct set up by allocStruct.
//
// CEF wrapper lifetimes NET-CONSUME one underlying reference per wrapper
// (Wrap's constructor AddRef is immediately compensated, but the wrapper
// destructor releases again), which is exactly how C++ clients' own
// CefRefPtrs behave. Anchored objects therefore clamp their refcount at
// one "anchor" reference held by this package for the process (or window)
// lifetime — CEF releases alone can never free them. Unanchored objects
// are freed when their refcount reaches zero, driven entirely by CEF's
// own acquire/release pairs.
func registerObject(self unsafe.Pointer, anchored bool, onFree func(unsafe.Pointer)) {
	o := &cefObject{anchored: anchored, onFree: onFree}
	// Both kinds start at 1: CEF wrapper lifetimes net-consume one
	// underlying reference, so unanchored (per-request) objects are
	// freed exactly when their last wrapper dies, while anchored
	// objects clamp at the anchor and outlive any number of wrappers.
	o.refs.Store(1)
	objects.Store(self, o)
}

// allocStruct allocates a zeroed, anchored CEF struct of the given size
// (use C.sizeof_<struct>), installs the shared refcount callbacks and
// registers it. Anchored structs live until process teardown; use for
// process- or window-lifetime handler objects.
func allocStruct(size C.size_t) unsafe.Pointer {
	p := unsafe.Pointer(C.calloc(1, size))
	C.wcef_init_base(p, size)
	registerObject(p, true, defaultFree)
	return p
}

// allocStructUnanchored allocates a CEF struct whose lifetime is driven
// purely by CEF's references; use for per-request objects such as
// resource handlers.
func allocStructUnanchored(size C.size_t) unsafe.Pointer {
	p := unsafe.Pointer(C.calloc(1, size))
	C.wcef_init_base(p, size)
	registerObject(p, false, defaultFree)
	return p
}

func defaultFree(self unsafe.Pointer) {
	C.free(self)
}

func objAddRef(self *C.cef_base_ref_counted_t) {
	if o, ok := objects.Load(unsafe.Pointer(self)); ok {
		o.(*cefObject).refs.Add(1)
	}
}

func objRelease(self *C.cef_base_ref_counted_t) C.int {
	o, ok := objects.Load(unsafe.Pointer(self))
	if !ok {
		return 0
	}
	obj := o.(*cefObject)
	newVal := obj.refs.Add(-1)
	if obj.anchored && newVal < 1 {
		// Restore the anchor; CEF releases never free anchored structs.
		obj.refs.Add(1)
		return 0
	}
	if newVal > 0 {
		return 0
	}
	objects.Delete(unsafe.Pointer(self))
	obj.onFree(unsafe.Pointer(self))
	return 1
}

func objHasOneRef(self *C.cef_base_ref_counted_t) C.int {
	if o, ok := objects.Load(unsafe.Pointer(self)); ok {
		if o.(*cefObject).refs.Load() == 1 {
			return 1
		}
	}
	return 0
}

func objHasAtLeastOneRef(self *C.cef_base_ref_counted_t) C.int {
	if o, ok := objects.Load(unsafe.Pointer(self)); ok {
		if o.(*cefObject).refs.Load() >= 1 {
			return 1
		}
	}
	return 0
}
