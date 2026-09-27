//go:build linux && wails_cef

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR} -DCEF_API_VERSION=15400
*/
import "C"

// wailsCEFAddRef & co are the cef_base_ref_counted_t callbacks installed on
// every struct created by allocStruct; see loader.go for the object
// registry backing them.

//export wailsCEFAddRef
func wailsCEFAddRef(self *C.cef_base_ref_counted_t) { objAddRef(self) }

//export wailsCEFRelease
func wailsCEFRelease(self *C.cef_base_ref_counted_t) C.int { return objRelease(self) }

//export wailsCEFHasOneRef
func wailsCEFHasOneRef(self *C.cef_base_ref_counted_t) C.int { return objHasOneRef(self) }

//export wailsCEFHasAtLeastOneRef
func wailsCEFHasAtLeastOneRef(self *C.cef_base_ref_counted_t) C.int {
	return objHasAtLeastOneRef(self)
}
