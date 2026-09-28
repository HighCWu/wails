#ifndef WAILS_CEF_GLUE_H
#define WAILS_CEF_GLUE_H

#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_callback_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_process_message_capi.h"
#include "include/capi/cef_request_capi.h"
#include "include/capi/cef_resource_handler_capi.h"
#include "include/capi/cef_response_capi.h"
#include "include/capi/cef_scheme_capi.h"
#include "include/capi/cef_v8_capi.h"
#include "include/internal/cef_string_multimap.h"

#if defined(OS_WIN)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Refcount shims over the Go-implemented cef_base_ref_counted_t callbacks
// (callbacks.go). Static definitions in this header give every
// translation unit identical copies; wcef_init_base installs them.
// ---------------------------------------------------------------------------

// Implemented in Go via cgo //export (callbacks.go).
void wailsCEFAddRef(cef_base_ref_counted_t* self);
int wailsCEFRelease(cef_base_ref_counted_t* self);
int wailsCEFHasOneRef(cef_base_ref_counted_t* self);
int wailsCEFHasAtLeastOneRef(cef_base_ref_counted_t* self);

static void wails_cef_add_ref(struct _cef_base_ref_counted_t* self) {
  wailsCEFAddRef(self);
}
static int wails_cef_release(struct _cef_base_ref_counted_t* self) {
  return wailsCEFRelease(self);
}
static int wails_cef_has_one_ref(struct _cef_base_ref_counted_t* self) {
  return wailsCEFHasOneRef(self);
}
static int wails_cef_has_at_least_one_ref(struct _cef_base_ref_counted_t* self) {
  return wailsCEFHasAtLeastOneRef(self);
}

// wcef_init_base installs the shared refcount callbacks on a freshly
// allocated CEF struct.
static void wcef_init_base(void* p, size_t size) {
  cef_base_ref_counted_t* base = (cef_base_ref_counted_t*)p;
  base->size = size;
  base->add_ref = wails_cef_add_ref;
  base->release = wails_cef_release;
  base->has_one_ref = wails_cef_has_one_ref;
  base->has_at_least_one_ref = wails_cef_has_at_least_one_ref;
}

// ---------------------------------------------------------------------------
// libcef symbol table + wrappers, implemented in cef_capi.c. Go cannot
// call C function pointers directly, so every dlsym'd entry is exposed as
// a plain C function here.
// ---------------------------------------------------------------------------

// wcef_load dlopens libcef and resolves the symbol table. Returns 1 on
// success; on failure returns 0 and wcef_load_error() describes the
// missing symbol (a static buffer, valid until the next wcef_load call).
int wcef_load(const char* libcef_path);
const char* wcef_load_error(void);

int wcef_execute_process(const cef_main_args_t* args, cef_app_t* app);
int wcef_initialize(const cef_main_args_t* args,
                    const cef_settings_t* settings,
                    cef_app_t* app);
void wcef_shutdown(void);
void wcef_do_message_loop_work(void);
int wcef_get_exit_code(void);

int wcef_create_browser(const cef_window_info_t* window_info,
                        cef_client_t* client,
                        const cef_string_t* url,
                        const cef_browser_settings_t* settings);

// Strings. wcef_string_utf8_to_utf16 writes into the caller-owned
// cef_string_t (allocated by libcef, freed via wcef_string_utf16_clear).
int wcef_string_utf8_to_utf16(const char* src, size_t src_len, cef_string_t* out);
void wcef_string_utf16_clear(cef_string_t* s);
int wcef_string_utf16_to_utf8(const char16_t* src, size_t src_len, cef_string_utf8_t* out);
void wcef_string_utf8_clear(cef_string_utf8_t* s);
cef_string_userfree_utf16_t wcef_string_userfree_utf16_alloc(void);
void wcef_string_userfree_utf16_free(cef_string_userfree_utf16_t s);

// String multimaps (HTTP headers).
cef_string_multimap_t wcef_string_multimap_alloc(void);
void wcef_string_multimap_free(cef_string_multimap_t map);
int wcef_string_multimap_append(cef_string_multimap_t map, const cef_string_t* key, const cef_string_t* value);
size_t wcef_string_multimap_size(cef_string_multimap_t map);
int wcef_string_multimap_key(cef_string_multimap_t map, size_t index, cef_string_t* key);
int wcef_string_multimap_value(cef_string_multimap_t map, size_t index, cef_string_t* value);

// Custom schemes.
int wcef_register_scheme_handler_factory(const cef_string_t* scheme_name,
                                         const cef_string_t* domain_name,
                                         cef_scheme_handler_factory_t* factory);

// Process messages (renderer ↔ browser IPC).
cef_process_message_t* wcef_process_message_create(const cef_string_t* name, cef_process_id_t target);

// V8 (render process only). CefRegisterExtension was removed in CEF API
// 15400; bindings are installed per-context from on_context_created.
cef_v8_context_t* wcef_v8_context_get_current(void);
cef_v8_value_t* wcef_v8_value_create_object(void);
cef_v8_value_t* wcef_v8_value_create_function(const cef_string_t* name, cef_v8_handler_t* handler);
int wcef_v8_value_set_bykey(cef_v8_value_t* obj, const cef_string_t* key, cef_v8_value_t* value);
void wcef_install_webview_bridge(cef_v8_value_t* global, cef_v8_value_t* fn);
cef_v8_value_t* wcef_v8ctx_get_global(cef_v8_context_t* ctx);

// ---------------------------------------------------------------------------
// Method-call wrappers. Go cannot call C struct function pointers, so
// every CEF method this package uses is exposed as a plain C function
// (implemented in cef_capi.c).
// ---------------------------------------------------------------------------

// Generic refcounting for CEF-provided objects we retain.
void wcef_obj_add_ref(void* obj);
int wcef_obj_release(void* obj);

// cef_command_line_t
void wcef_cl_append_switch(cef_command_line_t* cl, const cef_string_t* name);

// cef_scheme_registrar_t
int wcef_registrar_add_custom_scheme(cef_scheme_registrar_t* reg, const cef_string_t* name, int options);

// cef_process_message_t
cef_string_userfree_utf16_t wcef_msg_get_name(cef_process_message_t* m);
cef_list_value_t* wcef_msg_get_argument_list(cef_process_message_t* m);

// cef_list_value_t
size_t wcef_list_get_size(cef_list_value_t* l);
cef_string_userfree_utf16_t wcef_list_get_string(cef_list_value_t* l, size_t index);
int wcef_list_set_string(cef_list_value_t* l, size_t index, const cef_string_t* value);

// cef_v8_value_t
int wcef_v8v_is_string(cef_v8_value_t* v);
cef_string_userfree_utf16_t wcef_v8v_get_string_value(cef_v8_value_t* v);

// cef_v8_context_t
cef_frame_t* wcef_v8ctx_get_frame(cef_v8_context_t* ctx);

// cef_frame_t
cef_string_userfree_utf16_t wcef_frame_get_url(cef_frame_t* f);
int wcef_frame_is_main(cef_frame_t* f);
int wcef_frame_is_valid(cef_frame_t* f);
void wcef_frame_exec_js(cef_frame_t* f, const cef_string_t* code, const cef_string_t* script_url, int start_line);
void wcef_frame_send_message(cef_frame_t* f, cef_process_id_t target, cef_process_message_t* m);

// cef_browser_t
int wcef_browser_get_identifier(cef_browser_t* b);
cef_browser_host_t* wcef_browser_get_host(cef_browser_t* b);
cef_frame_t* wcef_browser_get_main_frame(cef_browser_t* b);
int wcef_browser_is_valid(cef_browser_t* b);

// cef_browser_host_t
cef_window_handle_t wcef_host_get_window_handle(cef_browser_host_t* h);
void wcef_host_close_browser(cef_browser_host_t* h, int force_close);
void wcef_host_set_focus(cef_browser_host_t* h);

// cef_request_t
cef_string_userfree_utf16_t wcef_request_get_url(cef_request_t* r);
cef_string_userfree_utf16_t wcef_request_get_method(cef_request_t* r);
cef_string_multimap_t wcef_request_get_header_map(cef_request_t* r);
cef_post_data_t* wcef_request_get_post_data(cef_request_t* r);

// cef_post_data_t / element
size_t wcef_pd_get_element_count(cef_post_data_t* p);
void wcef_pd_get_elements(cef_post_data_t* p, size_t* count, cef_post_data_element_t** elements);
int wcef_pde_get_type(cef_post_data_element_t* e);
size_t wcef_pde_get_bytes_count(cef_post_data_element_t* e);
size_t wcef_pde_get_bytes(cef_post_data_element_t* e, size_t size, void* bytes);

// cef_response_t
void wcef_response_set_status(cef_response_t* r, int status);
void wcef_response_set_status_text(cef_response_t* r, const cef_string_t* text);
void wcef_response_set_mime_type(cef_response_t* r, const cef_string_t* mime);
void wcef_response_set_header_map(cef_response_t* r, cef_string_multimap_t map);

// cef_frame_t (continued)
void wcef_frame_load_url(cef_frame_t* f, const cef_string_t* url);

// cef_browser_t (continued)
void wcef_browser_reload(cef_browser_t* b);
void wcef_browser_reload_ignore_cache(cef_browser_t* b);
void wcef_browser_stop_load(cef_browser_t* b);

// cef_browser_host_t (continued)
double wcef_host_get_zoom_level(cef_browser_host_t* h);
void wcef_host_set_zoom_level(cef_browser_host_t* h, double zoom_level);

// cef_browser_host_t (continued)
void wcef_host_close_dev_tools(cef_browser_host_t* h);
void wcef_host_show_dev_tools(cef_browser_host_t* h, cef_client_t* client);

// cef_drag_data_t (browser process, drag handler)
cef_string_list_t wcef_drag_data_get_file_paths(cef_drag_data_t* d);

// String lists (drag data file names).
cef_string_list_t wcef_string_list_alloc(void);
void wcef_string_list_free(cef_string_list_t list);
size_t wcef_string_list_size(cef_string_list_t list);
int wcef_string_list_value(cef_string_list_t list, size_t index, cef_string_t* value);

// cef_media_access_callback_t
void wcef_media_callback_cont(void* cb, uint32_t allowed_permissions);

// cef_callback_t
void wcef_callback_cont(cef_callback_t* cb);
void wcef_callback_cancel(cef_callback_t* cb);

#endif  // WAILS_CEF_GLUE_H

void wcef_main_args(cef_main_args_t* args, int argc, char** argv);
void wcef_window_parent(cef_window_info_t* info, uintptr_t parent);
uintptr_t wcef_native_handle(cef_browser_host_t* host);
