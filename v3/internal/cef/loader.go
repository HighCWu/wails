//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}
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
	refs   atomic.Int32
	onFree func(self unsafe.Pointer)
}

var objects sync.Map // unsafe.Pointer(C struct) -> *cefObject

// registerObject takes ownership of a struct set up by allocStruct. The
// caller holds the initial reference.
func registerObject(self unsafe.Pointer, onFree func(unsafe.Pointer)) {
	objects.Store(self, &cefObject{onFree: onFree})
}

// allocStruct allocates a zeroed CEF struct of the given size (use
// C.sizeof_<struct>), installs the shared refcount callbacks and registers
// it with a default free that releases the C memory.
func allocStruct(size C.size_t) unsafe.Pointer {
	p := unsafe.Pointer(C.calloc(1, size))
	C.wcef_init_base(p, size)
	registerObject(p, defaultFree)
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
	if obj.refs.Add(-1) > 0 {
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
