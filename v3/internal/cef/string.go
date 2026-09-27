//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}
*/
import "C"

import (
	"unsafe"
)

// cefString owns a cef_string_t whose buffer was allocated by libcef.
// It must be kept alive as long as the C side holds a reference to the
// underlying cef_string_t memory.
type cefString struct {
	c C.cef_string_t
}

// newCefString converts a Go string into an owned cef_string_t using
// libcef's own utf8→utf16 helper so allocation/dtor stay consistent with
// the runtime.
func newCefString(s string) *cefString {
	cs := &cefString{}
	var b []byte
	if len(s) > 0 {
		b = []byte(s)
	}
	var src *C.char
	if len(b) > 0 {
		src = (*C.char)(unsafe.Pointer(&b[0]))
	}
	C.wcef_string_utf8_to_utf16(src, C.size_t(len(b)), &cs.c)
	return cs
}

// ptr returns the *cef_string_t for passing to CEF APIs. The value
// remains owned by the Go side; CEF copies where documented.
func (s *cefString) ptr() *C.cef_string_t { return &s.c }

// Clear frees the underlying buffer (idempotent).
func (s *cefString) Clear() {
	C.wcef_string_utf16_clear(&s.c)
}

// goString converts a CEF-owned cef_string_t into a Go string (copied).
// The input is not modified or freed.
func goString(cs *C.cef_string_t) string {
	if cs == nil || cs.str == nil {
		return ""
	}
	// cef_string_t carries no length; utf16 payloads are NUL terminated.
	n := 0
	for p := (*C.char16_t)(cs.str); *p != 0; p = (*C.char16_t)(unsafe.Add(unsafe.Pointer(p), 2)) {
		n++
	}
	var out C.cef_string_utf8_t
	if C.wcef_string_utf16_to_utf8((*C.char16_t)(cs.str), C.size_t(n), &out) != 1 {
		return ""
	}
	defer C.wcef_string_utf8_clear(&out)
	if out.str == nil {
		return ""
	}
	return C.GoString((*C.char)(unsafe.Pointer(out.str)))
}

// userfreeToString reads and frees a cef_string_userfree_utf16_t (the
// return convention of most CEF string getters).
func userfreeToString(uf C.cef_string_userfree_utf16_t) string {
	if uf == nil {
		return ""
	}
	s := goString((*C.cef_string_t)(unsafe.Pointer(uf)))
	C.wcef_string_userfree_utf16_free(uf)
	return s
}
