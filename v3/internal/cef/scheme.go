//go:build linux

package cef

/*
#include "cef_glue.h"
#cgo CFLAGS: -I${SRCDIR}

// Shims over the //export'ed callbacks in scheme_handlers.go.

// cef_scheme_handler_factory_t
struct _cef_resource_handler_t* wailsCEFFactoryCreate(struct _cef_scheme_handler_factory_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, const cef_string_t* scheme_name, struct _cef_request_t* request);
static struct _cef_resource_handler_t* wails_cef_factory_create(struct _cef_scheme_handler_factory_t* self, struct _cef_browser_t* browser, struct _cef_frame_t* frame, const cef_string_t* scheme_name, struct _cef_request_t* request) {
  return wailsCEFFactoryCreate(self, browser, frame, scheme_name, request);
}

// cef_resource_handler_t
int wailsCEFResourceProcessRequest(struct _cef_resource_handler_t* self, struct _cef_request_t* request, struct _cef_callback_t* callback);
static int wails_cef_resource_process_request(struct _cef_resource_handler_t* self, struct _cef_request_t* request, struct _cef_callback_t* callback) {
  return wailsCEFResourceProcessRequest(self, request, callback);
}
void wailsCEFResourceGetResponseHeaders(struct _cef_resource_handler_t* self, struct _cef_response_t* response, int64_t* response_length, cef_string_t* redirect_url);
static void wails_cef_resource_get_response_headers(struct _cef_resource_handler_t* self, struct _cef_response_t* response, int64_t* response_length, cef_string_t* redirect_url) {
  wailsCEFResourceGetResponseHeaders(self, response, response_length, redirect_url);
}
int wailsCEFResourceReadResponse(struct _cef_resource_handler_t* self, void* data_out, int bytes_to_read, int* bytes_read, struct _cef_callback_t* callback);
static int wails_cef_resource_read_response(struct _cef_resource_handler_t* self, void* data_out, int bytes_to_read, int* bytes_read, struct _cef_callback_t* callback) {
  return wailsCEFResourceReadResponse(self, data_out, bytes_to_read, bytes_read, callback);
}
void wailsCEFResourceCancel(struct _cef_resource_handler_t* self);
static void wails_cef_resource_cancel(struct _cef_resource_handler_t* self) {
  wailsCEFResourceCancel(self);
}

// Struct initialisers (see app.go for why wiring happens in C).
static void wcef_init_factory(void* p) {
  cef_scheme_handler_factory_t* f = (cef_scheme_handler_factory_t*)p;
  f->create = wails_cef_factory_create;
}
static void wcef_init_resource_handler(void* p) {
  cef_resource_handler_t* h = (cef_resource_handler_t*)p;
  h->process_request = wails_cef_resource_process_request;
  h->get_response_headers = wails_cef_resource_get_response_headers;
  h->read_response = wails_cef_resource_read_response;
  h->cancel = wails_cef_resource_cancel;
}
*/
import "C"

import (
	"bytes"
	"io"
	"net/http"
	"strconv"
	"sync"
	"unsafe"

	"github.com/wailsapp/wails/v3/internal/assetserver/webview"
)

var theFactory *C.cef_scheme_handler_factory_t

// buildSchemeFactory creates the singleton wails:// factory.
func buildSchemeFactory() *C.cef_scheme_handler_factory_t {
	if theFactory != nil {
		return theFactory
	}
	f := (*C.cef_scheme_handler_factory_t)(allocStruct(C.sizeof_cef_scheme_handler_factory_t))
	C.wcef_init_factory(unsafe.Pointer(f))
	theFactory = f
	return f
}

// factoryCreate implements cef_scheme_handler_factory_t.create. Runs on
// the CEF IO thread; hands each request a resourceHandler bridging into
// the wails asset server.
func factoryCreate(browser *C.cef_browser_t, frame *C.cef_frame_t, scheme *C.cef_string_t, request *C.cef_request_t) *C.cef_resource_handler_t {
	_ = scheme
	h := newResourceHandler()
	// Retain references for the lifetime of the request handling.
	h.browser = browser
	h.frame = frame
	h.request = request
	C.wcef_obj_add_ref(unsafe.Pointer(browser))
	C.wcef_obj_add_ref(unsafe.Pointer(frame))
	C.wcef_obj_add_ref(unsafe.Pointer(request))

	if b := lookupBrowserByCefID(int(C.wcef_browser_get_identifier(browser))); b != nil {
		h.windowID = b.windowID
	}
	handlerRegistry.Store(unsafe.Pointer(h.c), h)
	return h.c
}

// ---------------------------------------------------------------------------
// resourceHandler + assetRequest + responseWriter: the request pipeline.
//
// process_request  →  assetRequest pushed to the wails asset server
//                     (goroutine side) via State.AssetRequest
// responseWriter   ←  written by the asset server goroutine; first write
//                     releases headers and continues the CEF request
// read_response    ←  drains the buffered body, async when empty
// ---------------------------------------------------------------------------

type resourceHandler struct {
	c *C.cef_resource_handler_t

	// CEF-owned objects retained between process_request and release.
	browser *C.cef_browser_t
	frame   *C.cef_frame_t
	request *C.cef_request_t

	windowID uint

	mu         sync.Mutex
	cancelled  bool
	headersSet bool
	done       bool // asset server finished the response
	status     int
	statusText string
	mimeType   string
	header     http.Header
	body       bytes.Buffer
	startCB    *C.cef_callback_t // request callback until headers ready
	readCB     *C.cef_callback_t // read callback while waiting for data
}

func newResourceHandler() *resourceHandler {
	h := &resourceHandler{}
	h.c = (*C.cef_resource_handler_t)(allocStruct(C.sizeof_cef_resource_handler_t))
	C.wcef_init_resource_handler(unsafe.Pointer(h.c))
	registerObject(unsafe.Pointer(h.c), func(p unsafe.Pointer) {
		handlerRegistry.Delete(p)
		h.releaseCEFRefs()
	})
	return h
}

func (h *resourceHandler) releaseCEFRefs() {
	if h.browser != nil {
		C.wcef_obj_release(unsafe.Pointer(h.browser))
	}
	if h.frame != nil {
		C.wcef_obj_release(unsafe.Pointer(h.frame))
	}
	if h.request != nil {
		C.wcef_obj_release(unsafe.Pointer(h.request))
	}
}

// handlerRegistry maps resource handler C pointers to their Go objects
// (the generic registry only stores refcount state).
var handlerRegistry sync.Map // unsafe.Pointer -> *resourceHandler

func handlerByPtr(p unsafe.Pointer) *resourceHandler {
	if v, ok := handlerRegistry.Load(p); ok {
		return v.(*resourceHandler)
	}
	return nil
}

// resourceProcessRequest implements cef_resource_handler_t.process_request.
func resourceProcessRequest(self *C.cef_resource_handler_t, request *C.cef_request_t, callback *C.cef_callback_t) C.int {
	h := handlerByPtr(unsafe.Pointer(self))
	if h == nil {
		return 0
	}

	st := state.Load()
	if st == nil || st.AssetRequest == nil {
		C.wcef_callback_cancel(callback)
		return 1
	}

	h.mu.Lock()
	if h.cancelled {
		h.mu.Unlock()
		C.wcef_callback_cancel(callback)
		return 1
	}
	h.startCB = callback
	C.wcef_obj_add_ref(unsafe.Pointer(callback))
	h.mu.Unlock()

	// Hand the request to the wails asset server; the consumer goroutine
	// writes the response into h's responseWriter (see assetRequest).
	st.AssetRequest(&AssetRequest{Request: newAssetRequest(h), WindowID: h.windowID})
	return 1
}

// responseReady continues the CEF request once headers are available.
func (h *resourceHandler) responseReady() {
	h.mu.Lock()
	cb := h.startCB
	h.startCB = nil
	h.mu.Unlock()
	if cb != nil {
		C.wcef_callback_cont(cb)
		C.wcef_obj_release(unsafe.Pointer(cb))
	}
}

// resourceGetResponseHeaders implements
// cef_resource_handler_t.get_response_headers.
func resourceGetResponseHeaders(self *C.cef_resource_handler_t, response *C.cef_response_t, responseLength *C.int64_t, redirectURL *C.cef_string_t) {
	_ = redirectURL
	h := handlerByPtr(unsafe.Pointer(self))
	if h == nil {
		*responseLength = 0
		return
	}

	h.mu.Lock()
	status, statusText, mime, header := h.status, h.statusText, h.mimeType, h.header
	if status == 0 {
		status = http.StatusOK
		statusText = http.StatusText(http.StatusOK)
	}
	h.mu.Unlock()

	C.wcef_response_set_status(response, C.int(status))
	if statusText != "" {
		s := newCefString(statusText)
		defer s.Clear()
		C.wcef_response_set_status_text(response, s.ptr())
	}
	if mime != "" {
		m := newCefString(mime)
		defer m.Clear()
		C.wcef_response_set_mime_type(response, m.ptr())
	}
	if header != nil {
		multimap := C.wcef_string_multimap_alloc()
		for k, vs := range header {
			for _, v := range vs {
				ck := newCefString(k)
				cv := newCefString(v)
				C.wcef_string_multimap_append(multimap, ck.ptr(), cv.ptr())
				ck.Clear()
				cv.Clear()
			}
		}
		C.wcef_response_set_header_map(response, multimap)
		C.wcef_string_multimap_free(multimap)
	}

	*responseLength = -1
	if header != nil {
		if cl := header.Get("Content-Length"); cl != "" {
			if n, err := strconv.ParseInt(cl, 10, 64); err == nil {
				*responseLength = C.int64_t(n)
			}
		}
	}
}

// resourceReadResponse implements cef_resource_handler_t.read_response.
func resourceReadResponse(self *C.cef_resource_handler_t, dataOut unsafe.Pointer, bytesToRead C.int, bytesRead *C.int, callback *C.cef_callback_t) C.int {
	h := handlerByPtr(unsafe.Pointer(self))
	if h == nil {
		return 0
	}

	h.mu.Lock()
	if h.cancelled {
		h.mu.Unlock()
		*bytesRead = 0
		return 0
	}
	if h.body.Len() > 0 {
		n := h.body.Len()
		if int(bytesToRead) < n {
			n = int(bytesToRead)
		}
		data := h.body.Next(n)
		copy(unsafe.Slice((*byte)(dataOut), n), data)
		*bytesRead = C.int(n)
		h.mu.Unlock()
		return 1
	}
	if h.done {
		h.mu.Unlock()
		*bytesRead = 0
		return 0
	}
	// No data yet: continue asynchronously when the next write arrives.
	h.readCB = callback
	C.wcef_obj_add_ref(unsafe.Pointer(callback))
	*bytesRead = 0
	h.mu.Unlock()
	return 1
}

// resourceCancel implements cef_resource_handler_t.cancel.
func resourceCancel(self *C.cef_resource_handler_t) {
	h := handlerByPtr(unsafe.Pointer(self))
	if h == nil {
		return
	}
	h.mu.Lock()
	h.cancelled = true
	cb := h.readCB
	h.readCB = nil
	start := h.startCB
	h.startCB = nil
	h.mu.Unlock()
	if cb != nil {
		C.wcef_callback_cancel(cb)
		C.wcef_obj_release(unsafe.Pointer(cb))
	}
	if start != nil {
		C.wcef_callback_cancel(start)
		C.wcef_obj_release(unsafe.Pointer(start))
	}
}

// write is called by the asset server goroutine via responseWriter.
func (h *resourceHandler) write(p []byte) (int, error) {
	h.mu.Lock()
	if h.cancelled || h.done {
		h.mu.Unlock()
		return 0, io.ErrClosedPipe
	}
	firstWrite := !h.headersSet
	h.headersSet = true
	n, _ := h.body.Write(p)
	cb := h.readCB
	h.readCB = nil
	h.mu.Unlock()

	if firstWrite {
		h.responseReady()
	}
	if cb != nil {
		C.wcef_callback_cont(cb)
		C.wcef_obj_release(unsafe.Pointer(cb))
	}
	return n, nil
}

// finish marks the response complete; further writes are refused.
func (h *resourceHandler) finish() {
	h.mu.Lock()
	if h.done {
		h.mu.Unlock()
		return
	}
	h.done = true
	h.headersSet = true
	cb := h.readCB
	h.readCB = nil
	h.mu.Unlock()

	h.responseReady()
	if cb != nil {
		C.wcef_callback_cont(cb)
		C.wcef_obj_release(unsafe.Pointer(cb))
	}
}

// ---------------------------------------------------------------------------
// assetRequest implements the wails webview.Request interface on top of a
// retained CEF request. Methods are called from the asset server
// goroutine.
// ---------------------------------------------------------------------------

type assetRequest struct {
	handler *resourceHandler
}

func newAssetRequest(h *resourceHandler) *assetRequest { return &assetRequest{handler: h} }

var _ webview.Request = (*assetRequest)(nil)

func (r *assetRequest) URL() (string, error) {
	return userfreeToString(C.wcef_request_get_url(r.handler.request)), nil
}

func (r *assetRequest) Method() (string, error) {
	return userfreeToString(C.wcef_request_get_method(r.handler.request)), nil
}

func (r *assetRequest) Header() (http.Header, error) {
	multimap := C.wcef_request_get_header_map(r.handler.request)
	defer C.wcef_string_multimap_free(multimap)

	header := http.Header{}
	size := int(C.wcef_string_multimap_size(multimap))
	var key, val C.cef_string_t
	for i := 0; i < size; i++ {
		if C.wcef_string_multimap_enumerate_key(multimap, C.size_t(i), 0, &key) != 1 {
			continue
		}
		for j := 0; C.wcef_string_multimap_enumerate_value(multimap, C.size_t(i), C.size_t(j), &val) == 1; j++ {
			header.Add(goString(&key), goString(&val))
		}
	}
	return header, nil
}

func (r *assetRequest) Body() (io.ReadCloser, error) {
	pd := C.wcef_request_get_post_data(r.handler.request)
	if pd == nil {
		return nil, nil
	}
	count := int(C.wcef_pd_get_element_count(pd))
	if count == 0 {
		return nil, nil
	}
	ptrSize := unsafe.Sizeof((*C.cef_post_data_element_t)(nil))
	elements := (*(*[1 << 20]*C.cef_post_data_element_t)(C.calloc(C.size_t(count), C.size_t(ptrSize))))[:count:count]
	defer C.free(unsafe.Pointer(&elements[0]))
	cCount := C.size_t(count)
	C.wcef_pd_get_elements(pd, &cCount, &elements[0])

	var body []byte
	for i := 0; i < int(cCount); i++ {
		el := elements[i]
		if el == nil || C.wcef_pde_get_type(el) != C.PDE_TYPE_BYTES {
			continue
		}
		n := int(C.wcef_pde_get_bytes_count(el))
		if n == 0 {
			continue
		}
		buf := C.calloc(C.size_t(n), 1)
		read := int(C.wcef_pde_get_bytes(el, C.size_t(n), buf))
		if read > 0 {
			body = append(body, unsafe.Slice((*byte)(buf), read)...)
		}
		C.free(buf)
	}
	if body == nil {
		return nil, nil
	}
	return io.NopCloser(bytes.NewReader(body)), nil
}

func (r *assetRequest) Response() webview.ResponseWriter {
	return &responseWriter{handler: r.handler}
}

func (r *assetRequest) Close() error {
	r.handler.finish()
	return nil
}

// responseWriter implements the wails webview.ResponseWriter interface;
// writes land in the resource handler's buffer and are pumped into CEF.
type responseWriter struct {
	handler *resourceHandler
}

var _ webview.ResponseWriter = (*responseWriter)(nil)

func (w *responseWriter) Header() http.Header {
	w.handler.mu.Lock()
	defer w.handler.mu.Unlock()
	if w.handler.header == nil {
		w.handler.header = http.Header{}
	}
	return w.handler.header
}

// captureStatus latches status/mime before the first write continues the
// request (get_response_headers runs right after).
func (w *responseWriter) captureStatusLocked() {
	if w.handler.status != 0 {
		return
	}
	w.handler.status = http.StatusOK
	w.handler.statusText = http.StatusText(http.StatusOK)
	if w.handler.header != nil {
		w.handler.mimeType = w.handler.header.Get("Content-Type")
	}
}

func (w *responseWriter) WriteHeader(status int) {
	w.handler.mu.Lock()
	if w.handler.status == 0 {
		w.handler.status = status
		w.handler.statusText = http.StatusText(status)
		if w.handler.header != nil {
			w.handler.mimeType = w.handler.header.Get("Content-Type")
		}
	}
	w.handler.mu.Unlock()
}

func (w *responseWriter) Write(p []byte) (int, error) {
	w.handler.mu.Lock()
	w.captureStatusLocked()
	w.handler.mu.Unlock()
	return w.handler.write(p)
}

func (w *responseWriter) Finish() error {
	w.handler.finish()
	return nil
}

func (w *responseWriter) Code() int {
	w.handler.mu.Lock()
	defer w.handler.mu.Unlock()
	if w.handler.status == 0 {
		return http.StatusOK
	}
	return w.handler.status
}
