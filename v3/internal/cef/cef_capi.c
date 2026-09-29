// wails: libcef symbol table and call wrappers.
//
// libcef is dlopen'd at runtime (never linked at build time) so binaries
// run unchanged on machines without a CEF distribution. Go cannot call C
// function pointers, so every resolved symbol is exposed as a plain C
// function declared in cef_glue.h and defined here.

#include "cef_glue.h"

#include <stdio.h>
#include <string.h>

typedef int (*wcef_execute_process_fn)(const cef_main_args_t*, cef_app_t*, void*);
typedef int (*wcef_initialize_fn)(const cef_main_args_t*, const cef_settings_t*, cef_app_t*, void*);
typedef void (*wcef_shutdown_fn)(void);
typedef void (*wcef_do_work_fn)(void);
typedef int (*wcef_get_exit_code_fn)(void);
typedef int (*wcef_create_browser_fn)(const cef_window_info_t*, cef_client_t*, const cef_string_t*, const cef_browser_settings_t*, cef_dictionary_value_t*, cef_request_context_t*);
typedef int (*wcef_string_utf8_to_utf16_fn)(const char*, size_t, cef_string_utf16_t*);
typedef int (*wcef_string_utf16_to_utf8_fn)(const char16_t*, size_t, cef_string_utf8_t*);
typedef void (*wcef_string_utf8_clear_fn)(cef_string_utf8_t*);
typedef void (*wcef_string_utf16_clear_fn)(cef_string_utf16_t*);
typedef cef_string_userfree_utf16_t (*wcef_string_userfree_utf16_alloc_fn)(void);
typedef void (*wcef_string_userfree_utf16_free_fn)(cef_string_userfree_utf16_t);
typedef cef_string_multimap_t (*wcef_string_multimap_alloc_fn)(void);
typedef void (*wcef_string_multimap_free_fn)(cef_string_multimap_t);
typedef int (*wcef_string_multimap_append_fn)(cef_string_multimap_t, const cef_string_t*, const cef_string_t*);
typedef size_t (*wcef_string_multimap_size_fn)(cef_string_multimap_t);

typedef cef_process_message_t* (*wcef_process_message_create_fn)(const cef_string_t*, cef_process_id_t);
typedef cef_string_list_t (*wcef_string_list_alloc_fn)(void);
typedef void (*wcef_string_list_free_fn)(cef_string_list_t);
typedef size_t (*wcef_string_list_size_fn)(cef_string_list_t);
typedef int (*wcef_string_list_value_fn)(cef_string_list_t, size_t, cef_string_t*);
typedef cef_v8_context_t* (*wcef_v8_context_get_current_fn)(void);
typedef cef_v8_value_t* (*wcef_v8_value_create_object_fn)(cef_v8_accessor_t*, cef_v8_interceptor_t*);
typedef cef_v8_value_t* (*wcef_v8_value_create_function_fn)(const cef_string_t*, cef_v8_handler_t*);
typedef int (*wcef_v8_value_set_bykey_fn)(cef_v8_value_t*, const cef_string_t*, cef_v8_value_t*, cef_v8_propertyattribute_t);
typedef int (*wcef_register_scheme_handler_factory_fn)(const cef_string_t*, const cef_string_t*, cef_scheme_handler_factory_t*);
typedef const char* (*wcef_api_hash_fn)(int, int);
static wcef_api_hash_fn g_api_hash;

static void* g_lib = NULL;
static char g_error[192];

static wcef_execute_process_fn g_execute_process;
static wcef_initialize_fn g_initialize;
static wcef_shutdown_fn g_shutdown;
static wcef_do_work_fn g_do_work;
#if defined(OS_WIN)
static void (*g_set_osmodal_loop)(int);
#endif
static wcef_get_exit_code_fn g_get_exit_code;
static wcef_create_browser_fn g_create_browser;
static wcef_string_utf8_to_utf16_fn g_str_u8u16;
static wcef_string_utf16_to_utf8_fn g_str_u16u8;
static wcef_string_utf8_clear_fn g_str_u8clear;
static wcef_string_utf16_clear_fn g_str_u16clear;
static wcef_string_userfree_utf16_alloc_fn g_uf_alloc;
static wcef_string_userfree_utf16_free_fn g_uf_free;
static wcef_string_multimap_alloc_fn g_mm_alloc;
static wcef_string_multimap_free_fn g_mm_free;
static wcef_string_multimap_append_fn g_mm_append;
static wcef_string_multimap_size_fn g_mm_size;
typedef int (*wcef_string_multimap_kv_fn)(cef_string_multimap_t, size_t, cef_string_t*);
static wcef_string_multimap_kv_fn g_mm_key;
static wcef_string_multimap_kv_fn g_mm_value;
static wcef_process_message_create_fn g_msg_create;
static wcef_v8_context_get_current_fn g_v8_current;
static wcef_v8_value_create_object_fn g_v8_create_object;
static wcef_v8_value_create_function_fn g_v8_create_function;
static wcef_v8_value_set_bykey_fn g_v8_set_bykey;
static wcef_register_scheme_handler_factory_fn g_register_factory;
static wcef_string_list_alloc_fn g_sl_alloc;
static wcef_string_list_free_fn g_sl_free;
static wcef_string_list_size_fn g_sl_size;
static wcef_string_list_value_fn g_sl_value;

// wcef_sym resolves a symbol, recording the first failure.
static void* wcef_sym(const char* name) {
  #if defined(OS_WIN)
  void* s = (void*)GetProcAddress((HMODULE)g_lib, name);
#else
  void* s = dlsym(g_lib, name);
#endif
  if (s == NULL) {
    snprintf(g_error, sizeof(g_error),
             "libcef.so does not export %s (runtime does not match the "
             "vendored CEF headers)",
             name);
  }
  return s;
}

int wcef_load(const char* libcef_path) {
  #if defined(OS_WIN)
  int n = MultiByteToWideChar(CP_UTF8, 0, libcef_path, -1, NULL, 0);
  wchar_t* path = calloc(n, sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, libcef_path, -1, path, n);
  g_lib = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  free(path);
#else
  g_lib = dlopen(libcef_path, RTLD_LAZY | RTLD_GLOBAL);
#endif
  if (g_lib == NULL) {
    snprintf(g_error, sizeof(g_error), "cannot dlopen %s", libcef_path);
    return 0;
  }
  g_execute_process = (wcef_execute_process_fn)wcef_sym("cef_execute_process");
  g_initialize = (wcef_initialize_fn)wcef_sym("cef_initialize");
  g_shutdown = (wcef_shutdown_fn)wcef_sym("cef_shutdown");
#if defined(OS_WIN)
  g_set_osmodal_loop = (void (*)(int))wcef_sym("cef_set_osmodal_loop");
  if (!g_set_osmodal_loop) return 0;
#endif
  g_do_work = (wcef_do_work_fn)wcef_sym("cef_do_message_loop_work");
  g_get_exit_code = (wcef_get_exit_code_fn)wcef_sym("cef_get_exit_code");
  g_create_browser = (wcef_create_browser_fn)wcef_sym("cef_browser_host_create_browser");
  g_str_u8u16 = (wcef_string_utf8_to_utf16_fn)wcef_sym("cef_string_utf8_to_utf16");
  g_str_u16u8 = (wcef_string_utf16_to_utf8_fn)wcef_sym("cef_string_utf16_to_utf8");
  g_str_u8clear = (wcef_string_utf8_clear_fn)wcef_sym("cef_string_utf8_clear");
  g_str_u16clear = (wcef_string_utf16_clear_fn)wcef_sym("cef_string_utf16_clear");
  g_uf_alloc = (wcef_string_userfree_utf16_alloc_fn)wcef_sym("cef_string_userfree_utf16_alloc");
  g_uf_free = (wcef_string_userfree_utf16_free_fn)wcef_sym("cef_string_userfree_utf16_free");
  g_mm_alloc = (wcef_string_multimap_alloc_fn)wcef_sym("cef_string_multimap_alloc");
  g_mm_free = (wcef_string_multimap_free_fn)wcef_sym("cef_string_multimap_free");
  g_mm_append = (wcef_string_multimap_append_fn)wcef_sym("cef_string_multimap_append");
  g_mm_size = (wcef_string_multimap_size_fn)wcef_sym("cef_string_multimap_size");
  g_mm_key = (wcef_string_multimap_kv_fn)wcef_sym("cef_string_multimap_key");
  g_mm_value = (wcef_string_multimap_kv_fn)wcef_sym("cef_string_multimap_value");
  g_msg_create = (wcef_process_message_create_fn)wcef_sym("cef_process_message_create");
  g_v8_current = (wcef_v8_context_get_current_fn)wcef_sym("cef_v8_context_get_current_context");
  g_v8_create_object = (wcef_v8_value_create_object_fn)wcef_sym("cef_v8_value_create_object");
  g_v8_create_function = (wcef_v8_value_create_function_fn)wcef_sym("cef_v8_value_create_function");

  g_register_factory = (wcef_register_scheme_handler_factory_fn)wcef_sym("cef_register_scheme_handler_factory");
  g_api_hash = (wcef_api_hash_fn)wcef_sym("cef_api_hash");
  g_sl_alloc = (wcef_string_list_alloc_fn)wcef_sym("cef_string_list_alloc");
  g_sl_free = (wcef_string_list_free_fn)wcef_sym("cef_string_list_free");
  g_sl_size = (wcef_string_list_size_fn)wcef_sym("cef_string_list_size");
  g_sl_value = (wcef_string_list_value_fn)wcef_sym("cef_string_list_value");

  if (g_api_hash != NULL) {
    // Configure the API version from the vendored headers BEFORE any
    // other CEF call — CEF 139+ validates it when C API structs are
    // first passed in (subsequent calls are a no-op). The returned hash
    // must match the vendored headers exactly, otherwise the runtime
    // binary is a different CEF version than the binding was compiled
    // against and every subsequent call would fail mysteriously.
    const char* hash = g_api_hash(CEF_API_VERSION, 0);
    if (hash == NULL) {
      snprintf(g_error, sizeof(g_error),
               "libcef.so does not support CEF API version %d; this binding "
               "is built for CEF %d — provide a matching CEF runtime",
               (int)CEF_API_VERSION, (int)CEF_API_VERSION);
      return 0;
    }
    if (strcmp(hash, CEF_API_HASH_PLATFORM) != 0) {
      snprintf(g_error, sizeof(g_error),
               "libcef.so API hash mismatch: runtime is a different CEF build "
               "than the vendored headers (expected %.12s..., got %.12s...) — "
               "provide the matching CEF runtime",
               CEF_API_HASH_PLATFORM, hash);
      return 0;
    }
  }

  if (g_execute_process == NULL || g_initialize == NULL || g_shutdown == NULL ||
      g_get_exit_code == NULL || g_create_browser == NULL || g_str_u8u16 == NULL ||
      g_str_u16u8 == NULL || g_str_u8clear == NULL || g_str_u16clear == NULL ||
      g_uf_alloc == NULL || g_uf_free == NULL || g_mm_alloc == NULL ||
      g_mm_free == NULL || g_mm_append == NULL || g_mm_size == NULL ||
      g_mm_key == NULL || g_mm_value == NULL || g_msg_create == NULL ||
      g_v8_current == NULL || g_v8_create_object == NULL || g_v8_create_function == NULL ||
      g_register_factory == NULL) {
    return 0;
  }
  return 1;
}

const char* wcef_load_error(void) { return g_error; }

int wcef_execute_process(const cef_main_args_t* args, cef_app_t* app) {
  return g_execute_process(args, app, NULL);
}

int wcef_initialize(const cef_main_args_t* args, const cef_settings_t* settings, cef_app_t* app) {
  return g_initialize(args, settings, app, NULL);
}

void wcef_shutdown(void) { g_shutdown(); }

void wcef_do_message_loop_work(void) { g_do_work(); }

int wcef_get_exit_code(void) { return g_get_exit_code(); }

int wcef_create_browser(const cef_window_info_t* window_info, cef_client_t* client,
                        const cef_string_t* url, const cef_browser_settings_t* settings) {
  return g_create_browser(window_info, client, url, settings, NULL, NULL);
}

int wcef_string_utf8_to_utf16(const char* src, size_t src_len, cef_string_t* out) {
  return g_str_u8u16(src, src_len, out);
}

void wcef_string_utf16_clear(cef_string_t* s) { g_str_u16clear(s); }

int wcef_string_utf16_to_utf8(const char16_t* src, size_t src_len, cef_string_utf8_t* out) {
  return g_str_u16u8(src, src_len, out);
}

void wcef_string_utf8_clear(cef_string_utf8_t* s) { g_str_u8clear(s); }

cef_string_userfree_utf16_t wcef_string_userfree_utf16_alloc(void) { return g_uf_alloc(); }

void wcef_string_userfree_utf16_free(cef_string_userfree_utf16_t s) { g_uf_free(s); }

cef_string_multimap_t wcef_string_multimap_alloc(void) { return g_mm_alloc(); }

void wcef_string_multimap_free(cef_string_multimap_t map) { g_mm_free(map); }

int wcef_string_multimap_append(cef_string_multimap_t map, const cef_string_t* key, const cef_string_t* value) {
  return g_mm_append(map, key, value);
}

size_t wcef_string_multimap_size(cef_string_multimap_t map) { return g_mm_size(map); }

int wcef_string_multimap_key(cef_string_multimap_t map, size_t index, cef_string_t* key) {
  return g_mm_key(map, index, key);
}

int wcef_string_multimap_value(cef_string_multimap_t map, size_t index, cef_string_t* value) {
  return g_mm_value(map, index, value);
}

int wcef_register_scheme_handler_factory(const cef_string_t* scheme_name, const cef_string_t* domain_name,
                                         cef_scheme_handler_factory_t* factory) {
  return g_register_factory(scheme_name, domain_name, factory);
}

cef_process_message_t* wcef_process_message_create(const cef_string_t* name, cef_process_id_t target) {
  return g_msg_create(name, target);
}

cef_v8_context_t* wcef_v8_context_get_current(void) { return g_v8_current(); }

cef_v8_value_t* wcef_v8_value_create_object(void) { return g_v8_create_object(NULL, NULL); }

cef_v8_value_t* wcef_v8_value_create_function(const cef_string_t* name, cef_v8_handler_t* handler) {
  return g_v8_create_function(name, handler);
}

int wcef_v8_value_set_bykey(cef_v8_value_t* obj, const cef_string_t* key, cef_v8_value_t* value) {
  // CEF Unwrap consumes one reference for non-self object arguments.
  // Keep the caller-owned value alive until its explicit release.
  value->base.add_ref(&value->base);
  return obj->set_value_bykey(obj, key, value, V8_PROPERTY_ATTRIBUTE_NONE);
}

// Use the existing WebView2-compatible transport. runtime.js replaces
// window.wails with its public exports, so an invoke property there is lost.
void wcef_install_webview_bridge(cef_v8_value_t* global, cef_v8_value_t* fn) {
  cef_string_t chrome_key = {0}, webview_key = {0}, post_key = {0};
  g_str_u8u16("chrome", 6, &chrome_key);
  g_str_u8u16("webview", 7, &webview_key);
  g_str_u8u16("postMessage", 11, &post_key);
  cef_v8_value_t* chrome = global->get_value_bykey(global, &chrome_key);
  if (!chrome || !chrome->is_object(chrome)) {
    if (chrome) chrome->base.release(&chrome->base);
    chrome = g_v8_create_object(NULL, NULL);
  }
  cef_v8_value_t* webview = g_v8_create_object(NULL, NULL);
  wcef_v8_value_set_bykey(webview, &post_key, fn);
#if !defined(OS_LINUX)
  // Reuse the runtime's file-drop transport; OS paths come from OnDragEnter,
  // never from JS File names or a file:// navigation.
  cef_string_t drop_key = {0};
  g_str_u8u16("postMessageWithAdditionalObjects", 32, &drop_key);
  wcef_v8_value_set_bykey(webview, &drop_key, fn);
  g_str_u16clear(&drop_key);
#endif
  wcef_v8_value_set_bykey(chrome, &webview_key, webview);
  wcef_v8_value_set_bykey(global, &chrome_key, chrome);
  webview->base.release(&webview->base);
  chrome->base.release(&chrome->base);
  g_str_u16clear(&chrome_key);
  g_str_u16clear(&webview_key);
  g_str_u16clear(&post_key);
}

cef_v8_value_t* wcef_v8ctx_get_global(cef_v8_context_t* ctx) { return ctx->get_global(ctx); }

// ---------------------------------------------------------------------------
// Method-call wrappers.
// ---------------------------------------------------------------------------

void wcef_obj_add_ref(void* obj) {
  cef_base_ref_counted_t* base = (cef_base_ref_counted_t*)obj;
  if (base != NULL && base->add_ref != NULL) base->add_ref(base);
}

int wcef_obj_release(void* obj) {
  cef_base_ref_counted_t* base = (cef_base_ref_counted_t*)obj;
  if (base == NULL || base->release == NULL) return 0;
  return base->release(base);
}

void wcef_cl_append_switch(cef_command_line_t* cl, const cef_string_t* name) {
  cl->append_switch(cl, name);
}
void wcef_cl_append_switch_value(cef_command_line_t* cl, const cef_string_t* name, const cef_string_t* value) {
  cl->append_switch_with_value(cl, name, value);
}

int wcef_registrar_add_custom_scheme(cef_scheme_registrar_t* reg, const cef_string_t* name, int options) {
  return reg->add_custom_scheme(reg, name, options);
}

cef_string_userfree_utf16_t wcef_msg_get_name(cef_process_message_t* m) {
  return m->get_name(m);
}

cef_list_value_t* wcef_msg_get_argument_list(cef_process_message_t* m) {
  return m->get_argument_list(m);
}

size_t wcef_list_get_size(cef_list_value_t* l) { return l->get_size(l); }

cef_string_userfree_utf16_t wcef_list_get_string(cef_list_value_t* l, size_t index) {
  return l->get_string(l, index);
}

int wcef_list_set_string(cef_list_value_t* l, size_t index, const cef_string_t* value) {
  return l->set_string(l, index, value);
}

int wcef_v8v_is_string(cef_v8_value_t* v) { return v->is_string(v); }

cef_string_userfree_utf16_t wcef_v8v_get_string_value(cef_v8_value_t* v) {
  return v->get_string_value(v);
}

cef_frame_t* wcef_v8ctx_get_frame(cef_v8_context_t* ctx) { return ctx->get_frame(ctx); }

cef_string_userfree_utf16_t wcef_frame_get_url(cef_frame_t* f) { return f->get_url(f); }

int wcef_frame_is_main(cef_frame_t* f) { return f->is_main(f); }

int wcef_frame_is_valid(cef_frame_t* f) { return f->is_valid(f); }

void wcef_frame_exec_js(cef_frame_t* f, const cef_string_t* code, const cef_string_t* script_url, int start_line) {
  f->execute_java_script(f, code, script_url, start_line);
}

void wcef_frame_send_message(cef_frame_t* f, cef_process_id_t target, cef_process_message_t* m) {
  f->send_process_message(f, target, m);
}

int wcef_browser_get_identifier(cef_browser_t* b) { return b->get_identifier(b); }

cef_browser_host_t* wcef_browser_get_host(cef_browser_t* b) { return b->get_host(b); }

cef_frame_t* wcef_browser_get_main_frame(cef_browser_t* b) { return b->get_main_frame(b); }

int wcef_browser_is_valid(cef_browser_t* b) { return b->is_valid(b); }

cef_window_handle_t wcef_host_get_window_handle(cef_browser_host_t* h) {
  return h->get_window_handle(h);
}

void wcef_host_close_browser(cef_browser_host_t* h, int force_close) {
  h->close_browser(h, force_close);
}

void wcef_host_set_focus(cef_browser_host_t* h) { h->set_focus(h, 1); }

void wcef_host_was_resized(cef_browser_host_t* h) { h->was_resized(h); }

void wcef_host_send_mouse_move(cef_browser_host_t* h, int x, int y,
                               int modifiers, int leave) {
  cef_mouse_event_t e = {0};
  e.x = x;
  e.y = y;
  e.modifiers = (uint32_t)modifiers;
  h->send_mouse_move_event(h, &e, leave);
}

void wcef_host_send_mouse_click(cef_browser_host_t* h, int x, int y,
                                int modifiers, int button, int up, int count) {
  cef_mouse_event_t e = {0};
  e.x = x;
  e.y = y;
  e.modifiers = (uint32_t)modifiers;
  h->send_mouse_click_event(h, &e, (cef_mouse_button_type_t)button, up, count);
}

void wcef_host_send_mouse_wheel(cef_browser_host_t* h, int x, int y,
                                int modifiers, int delta_x, int delta_y) {
  cef_mouse_event_t e = {0};
  e.x = x;
  e.y = y;
  e.modifiers = (uint32_t)modifiers;
  h->send_mouse_wheel_event(h, &e, delta_x, delta_y);
}

void wcef_host_send_key_event(cef_browser_host_t* h, int key_type,
                              int modifiers, int windows_key_code,
                              int native_key_code, unsigned short character,
                              unsigned short unmodified_character,
                              int is_system_key, int focus_on_editable_field) {
  cef_key_event_t e = {0};
  e.type = (cef_key_event_type_t)key_type;
  e.modifiers = (uint32_t)modifiers;
  e.windows_key_code = windows_key_code;
  e.native_key_code = native_key_code;
  e.character = character;
  e.unmodified_character = unmodified_character;
  e.is_system_key = is_system_key;
  e.focus_on_editable_field = focus_on_editable_field;
  h->send_key_event(h, &e);
}

cef_string_userfree_utf16_t wcef_request_get_url(cef_request_t* r) { return r->get_url(r); }

cef_string_userfree_utf16_t wcef_request_get_method(cef_request_t* r) { return r->get_method(r); }

cef_string_multimap_t wcef_request_get_header_map(cef_request_t* r) {
  cef_string_multimap_t map = g_mm_alloc();
  r->get_header_map(r, map);
  return map;
}

cef_post_data_t* wcef_request_get_post_data(cef_request_t* r) { return r->get_post_data(r); }

size_t wcef_pd_get_element_count(cef_post_data_t* p) { return p->get_element_count(p); }

void wcef_pd_get_elements(cef_post_data_t* p, size_t* count, cef_post_data_element_t** elements) {
  p->get_elements(p, count, elements);
}

int wcef_pde_get_type(cef_post_data_element_t* e) { return e->get_type(e); }

size_t wcef_pde_get_bytes_count(cef_post_data_element_t* e) { return e->get_bytes_count(e); }

size_t wcef_pde_get_bytes(cef_post_data_element_t* e, size_t size, void* bytes) {
  return e->get_bytes(e, size, bytes);
}

void wcef_response_set_status(cef_response_t* r, int status) { r->set_status(r, status); }

void wcef_response_set_status_text(cef_response_t* r, const cef_string_t* text) {
  r->set_status_text(r, text);
}

void wcef_response_set_mime_type(cef_response_t* r, const cef_string_t* mime) {
  r->set_mime_type(r, mime);
}

void wcef_response_set_header_map(cef_response_t* r, cef_string_multimap_t map) {
  r->set_header_map(r, map);
}

void wcef_frame_load_url(cef_frame_t* f, const cef_string_t* url) { f->load_url(f, url); }

void wcef_browser_reload(cef_browser_t* b) { b->reload(b); }

void wcef_browser_reload_ignore_cache(cef_browser_t* b) { b->reload_ignore_cache(b); }

void wcef_browser_stop_load(cef_browser_t* b) { b->stop_load(b); }

double wcef_host_get_zoom_level(cef_browser_host_t* h) { return h->get_zoom_level(h); }

void wcef_host_set_zoom_level(cef_browser_host_t* h, double zoom_level) { h->set_zoom_level(h, zoom_level); }

void wcef_host_close_dev_tools(cef_browser_host_t* h) { h->close_dev_tools(h); }

void wcef_host_show_dev_tools(cef_browser_host_t* h, cef_client_t* client) {
  cef_window_info_t wi = {0};
  wi.size = sizeof(wi);
  wi.bounds.width = 900;
  wi.bounds.height = 640;
  cef_browser_settings_t settings = {0};
  settings.size = sizeof(settings);
  h->show_dev_tools(h, &wi, client, &settings, NULL);
}

cef_string_list_t wcef_drag_data_get_file_paths(cef_drag_data_t* d) {
  cef_string_list_t list = g_sl_alloc();
  d->get_file_paths(d, list);
  return list;
}

cef_string_list_t wcef_string_list_alloc(void) { return g_sl_alloc(); }

void wcef_string_list_free(cef_string_list_t list) { g_sl_free(list); }

size_t wcef_string_list_size(cef_string_list_t list) { return g_sl_size(list); }

int wcef_string_list_value(cef_string_list_t list, size_t index, cef_string_t* value) {
  return g_sl_value(list, index, value);
}

void wcef_media_callback_cont(void* cb, uint32_t allowed_permissions) {
  cef_media_access_callback_t* c = (cef_media_access_callback_t*)cb;
  c->cont(c, allowed_permissions);
}

void wcef_callback_cont(cef_callback_t* cb) { cb->cont(cb); }

void wcef_callback_cancel(cef_callback_t* cb) { cb->cancel(cb); }

void wcef_main_args(cef_main_args_t* args, int argc, char** argv) {
#if defined(OS_WIN)
 args->instance = GetModuleHandleW(NULL);
#else
 args->argc = argc; args->argv = argv;
#endif
}
void wcef_window_parent(cef_window_info_t* info, uintptr_t parent) {
#if defined(OS_MAC)
 info->parent_view = (cef_window_handle_t)parent;
#elif defined(OS_WIN)
 info->parent_window = (cef_window_handle_t)parent;
 info->style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
#else
 info->parent_window = (cef_window_handle_t)parent;
#endif
}
uintptr_t wcef_native_handle(cef_browser_host_t* host) { return (uintptr_t)host->get_window_handle(host); }

#if defined(OS_WIN)
void wcef_set_osmodal_loop(int active) { g_set_osmodal_loop(active); }
#endif

void wcef_record_media_permission(cef_browser_t* browser, const cef_string_t* origin, uint32_t permissions) {
  cef_browser_host_t* host = browser->get_host(browser);
  if (!host) return;
  cef_request_context_t* context = host->get_request_context(host);
  host->base.release(&host->base);
  if (!context) return;
  // Scope Chromium's permission subscription to the origin whose request the
  // host approved. The CEF permission handler still decides every gUM request.
  if (permissions & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE)
    context->set_content_setting(context, origin, origin, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC, CEF_CONTENT_SETTING_VALUE_ALLOW);
  if (permissions & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE)
    context->set_content_setting(context, origin, origin, CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA, CEF_CONTENT_SETTING_VALUE_ALLOW);
  wcef_obj_release(context);
}
