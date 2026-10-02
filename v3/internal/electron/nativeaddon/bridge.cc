// The electron backend's native bridge addon — pure N-API, both surfaces:
//   preloadInit(electron, v8)  — renderer: postMessage shim, file-drop
//                                contract, fast wire serde (C-side
//                                ValueSerializer format with injected-JS
//                                fallback), the fetch override, the
//                                named-pipe/UDS data plane;
//   mainEntry(electron, cfg)   — main process: the stdio JSON-lines
//                                control protocol, BrowserWindow
//                                creation/wiring, the stdin reader
//                                thread, orphan guards, and (linux) the
//                                X11 frameless drag/resize loop.
// Replaces the v8-direct bridge: electron.exe on Windows exports only
// the N-API surface (no node.lib for it either), so v8 C++ addons are
// not loadable there — and the N-API build needs no Electron-version
// pinning at all. The main surface is the old v8 mainEntry ported 1:1;
// wire serde stays byte-compatible with the Go decoder (node wraps
// typed arrays in the same 0x5C host-object form the Go codec speaks).
//
// Clone discipline: values cross as opaque handles; serialized bytes
// are written to the fd straight from the encode buffer, responses are
// decoded straight from the input (single copy each way).

#include <node_api.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#include <pthread.h>

#define UV_EXTERN
#include <uv.h>
#undef UV_EXTERN

#include <cerrno>
#include <cstdarg>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define BRIDGE_TIMEOUT_SEC 10

static int g_fd = -1;
static int g_connected = 0;
static pthread_mutex_t g_io_mu = PTHREAD_MUTEX_INITIALIZER;

// persistent references (single preload environment)
static napi_env g_env = nullptr;
static napi_ref g_ipc_renderer = nullptr;
static napi_ref g_webutils = nullptr;
static napi_ref g_orig_fetch = nullptr;
static napi_ref g_response_ctor = nullptr;
static napi_ref g_v8mod = nullptr;      // require('v8') — recv for serde calls
static napi_ref g_v8ser = nullptr;      // v8.serialize
static napi_ref g_v8des = nullptr;      // v8.deserialize
static napi_ref g_objproto = nullptr;   // Object.prototype — exotic gate

// forward declarations (callback wiring order)
static napi_value FetchOverride(napi_env env, napi_callback_info info);
static napi_value NativeInitThunk(napi_env env, napi_callback_info info);
static napi_value PostMessageAdditionalCb(napi_env env,
                                          napi_callback_info info);
static napi_value NativeEcho(napi_env env, napi_callback_info info);
static napi_value NativeInvoke(napi_env env, napi_callback_info info);

// ----------------------------------------------------------------------
// Windows napi dynamic binding. mingw cannot link against electron.exe's
// exports without an MSVC-style import library (dlltool def-file import
// libs are unreliable across binutils builds, and mingw ld ignores
// short-import archives), so every napi call is forwarded through a
// function pointer bound once at library load from the host module —
// electron.exe exports the full N-API surface. Call sites stay unchanged.
#ifdef _WIN32
#define NAPI_UV_LIST(X) \
  X(uv_async_init, (uv_loop_t* loop, uv_async_t* async, uv_async_cb cb), (loop, async, cb)) \
  X(uv_async_send, (uv_async_t* async), (async))
#define NAPI_DYN_LIST(X) \
  X(napi_call_function, (napi_env env, napi_value recv, napi_value func, size_t argc, const napi_value* argv, napi_value* result), (env, recv, func, argc, argv, result)) \
  X(napi_coerce_to_string, (napi_env env, napi_value value, napi_value* result), (env, value, result)) \
  X(napi_create_array_with_length, (napi_env env, size_t length, napi_value* result), (env, length, result)) \
  X(napi_create_buffer, (napi_env env, size_t length, void** data, napi_value* result), (env, length, data, result)) \
  X(napi_create_external_arraybuffer, (napi_env env, void* external_data, size_t byte_length, node_api_basic_finalize finalize_cb, void* finalize_hint, napi_value* result), (env, external_data, byte_length, finalize_cb, finalize_hint, result)) \
  X(napi_create_function, (napi_env env, const char* utf8name, size_t length, napi_callback cb, void* data, napi_value* result), (env, utf8name, length, cb, data, result)) \
  X(napi_create_int32, (napi_env env, int32_t value, napi_value* result), (env, value, result)) \
  X(napi_create_string_latin1, (napi_env env, const char* str, size_t length, napi_value* result), (env, str, length, result)) \
  X(napi_get_value_string_latin1, (napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result), (env, value, buf, bufsize, result)) \
  X(napi_create_object, (napi_env env, napi_value* result), (env, result)) \
  X(napi_create_reference, (napi_env env, napi_value value, uint32_t initial_refcount, napi_ref* result), (env, value, initial_refcount, result)) \
  X(napi_create_string_utf8, (napi_env env, const char* str, size_t length, napi_value* result), (env, str, length, result)) \
  X(napi_create_typedarray, (napi_env env, napi_typedarray_type type, size_t length, napi_value arraybuffer, size_t byte_offset, napi_value* result), (env, type, length, arraybuffer, byte_offset, result)) \
  X(napi_get_array_length, (napi_env env, napi_value value, uint32_t* result), (env, value, result)) \
  X(napi_get_boolean, (napi_env env, bool value, napi_value* result), (env, value, result)) \
  X(napi_get_buffer_info, (napi_env env, napi_value value, void** data, size_t* length), (env, value, data, length)) \
  X(napi_get_cb_info, (napi_env env, napi_callback_info cbinfo, size_t* argc, napi_value* argv, napi_value* this_arg, void** data), (env, cbinfo, argc, argv, this_arg, data)) \
  X(napi_get_element, (napi_env env, napi_value object, uint32_t index, napi_value* result), (env, object, index, result)) \
  X(napi_get_global, (napi_env env, napi_value* result), (env, result)) \
  X(napi_get_named_property, (napi_env env, napi_value object, const char* utf8name, napi_value* result), (env, object, utf8name, result)) \
  X(napi_get_null, (napi_env env, napi_value* result), (env, result)) \
  X(napi_get_prototype, (napi_env env, napi_value object, napi_value* result), (env, object, result)) \
  X(napi_get_reference_value, (napi_env env, napi_ref ref, napi_value* result), (env, ref, result)) \
  X(napi_get_typedarray_info, (napi_env env, napi_value typedarray, napi_typedarray_type* type, size_t* length, void** data, napi_value* arraybuffer, size_t* byte_offset), (env, typedarray, type, length, data, arraybuffer, byte_offset)) \
  X(napi_get_undefined, (napi_env env, napi_value* result), (env, result)) \
  X(napi_get_value_double, (napi_env env, napi_value value, double* result), (env, value, result)) \
  X(napi_get_value_string_utf8, (napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result), (env, value, buf, bufsize, result)) \
  X(napi_is_array, (napi_env env, napi_value value, bool* result), (env, value, result)) \
  X(napi_is_arraybuffer, (napi_env env, napi_value value, bool* result), (env, value, result)) \
  X(napi_is_date, (napi_env env, napi_value value, bool* result), (env, value, result)) \
  X(napi_is_exception_pending, (napi_env env, bool* result), (env, result)) \
  X(napi_is_typedarray, (napi_env env, napi_value value, bool* result), (env, value, result)) \
  X(napi_new_instance, (napi_env env, napi_value constructor, size_t argc, const napi_value* argv, napi_value* result), (env, constructor, argc, argv, result)) \
  X(napi_set_element, (napi_env env, napi_value object, uint32_t index, napi_value value), (env, object, index, value)) \
  X(napi_set_named_property, (napi_env env, napi_value object, const char* utf8name, napi_value value), (env, object, utf8name, value)) \
  X(napi_strict_equals, (napi_env env, napi_value lhs, napi_value rhs, bool* result), (env, lhs, rhs, result)) \
  X(napi_throw_error, (napi_env env, const char* code, const char* msg), (env, code, msg)) \
  X(napi_create_double, (napi_env env, double value, napi_value* result), (env, value, result)) \
  X(napi_get_property, (napi_env env, napi_value object, napi_value key, napi_value* result), (env, object, key, result)) \
  X(napi_get_property_names, (napi_env env, napi_value object, napi_value* result), (env, object, result)) \
  X(napi_get_value_bool, (napi_env env, napi_value value, bool* result), (env, value, result)) \
  X(napi_get_value_string_utf16, (napi_env env, napi_value value, char16_t* buf, size_t bufsize, size_t* result), (env, value, buf, bufsize, result)) \
  X(napi_typeof, (napi_env env, napi_value value, napi_valuetype* result), (env, value, result)) \
  X(napi_get_uv_event_loop, (napi_env env, uv_loop_t** loop), (env, loop)) \
  X(napi_open_handle_scope, (napi_env env, napi_handle_scope* result), (env, result)) \
  X(napi_close_handle_scope, (napi_env env, napi_handle_scope scope), (env, scope)) \
  X(napi_create_uint32, (napi_env env, uint32_t value, napi_value* result), (env, value, result)) \
  X(napi_get_and_clear_last_exception, (napi_env env, napi_value* result), (env, result)) \
  X(napi_delete_reference, (napi_env env, napi_ref ref), (env, ref)) \
  X(napi_is_promise, (napi_env env, napi_value value, bool* result), (env, value, result))

extern "C" {
#define X(name, params, args)            \
  static napi_status (*name##_p) params; \
  napi_status name params { return name##_p args; }
NAPI_DYN_LIST(X)
#undef X
#define X(name, params, args)   \
  static int (*name##_p) params; \
  int name params { return name##_p args; }
NAPI_UV_LIST(X)
#undef X
}

__attribute__((constructor)) static void napi_dyn_init() {
  HMODULE h = GetModuleHandleW(NULL);  // the host: electron.exe
#define X(name, params, args)                                  \
  name##_p = reinterpret_cast<napi_status(*) params>(           \
      GetProcAddress(h, #name));
  NAPI_DYN_LIST(X)
#undef X
#define X(name, params, args)                \
  name##_p = reinterpret_cast<int(*) params>(  \
      GetProcAddress(h, #name));
  NAPI_UV_LIST(X)
#undef X
}
#endif

// ----------------------------------------------------------------------
// small napi helpers

static napi_value NF(napi_env env, const char* s) {
  napi_value out = nullptr;
  napi_create_string_utf8(env, s, NAPI_AUTO_LENGTH, &out);
  return out;
}

static void ThrowE(napi_env env, const std::string& msg) {
  napi_throw_error(env, nullptr, msg.c_str());
}

// String::Utf8Value equivalent (two-call napi_get_value_string_utf8).
static bool U8(napi_env env, napi_value v, std::string* out) {
  out->clear();
  if (!v) return false;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, v, &t);
  if (t != napi_string) return false;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) {
    return false;
  }
  out->resize(len);
  if (len > 0 &&
      napi_get_value_string_utf8(env, v, &(*out)[0], len + 1, &len) != napi_ok) {
    out->clear();
    return false;
  }
  return true;
}

static napi_value GetProp(napi_env env, napi_value obj, const char* name) {
  napi_value out = nullptr;
  if (!obj || napi_get_named_property(env, obj, name, &out) != napi_ok) {
    return nullptr;
  }
  return out;
}

static bool SetProp(napi_env env, napi_value obj, const char* name,
                    napi_value v) {
  return obj && v && napi_set_named_property(env, obj, name, v) == napi_ok;
}

// napi_get_reference_value that returns nullptr on missing refs.
static napi_value RefV(napi_ref ref) {
  if (!ref) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(g_env, ref, &out) != napi_ok) return nullptr;
  return out;
}

// Call fn(recv, ...args); returns nullptr when the call itself failed
// (exception pending) so callers can bail without clobbering the throw.
static napi_value CallFn(napi_value fn, napi_value recv, size_t argc,
                         const napi_value* argv) {
  napi_value out = nullptr;
  if (!fn || !recv ||
      napi_call_function(g_env, recv, fn, argc, argv, &out) != napi_ok) {
    return nullptr;
  }
  return out;
}

static void Warn(const char* msg) { fprintf(stderr, "[go-bridge] %s\n", msg); }

// ----------------------------------------------------------------------
// wire I/O — identical frames to bridge.cc (4-byte LE length + payload)

static int write_all(int fd, const char* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return errno;
    }
    off += static_cast<size_t>(n);
  }
  return 0;
}

static int read_full(int fd, char* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return errno;
    }
    if (n == 0) return ECONNRESET;
    off += static_cast<size_t>(n);
  }
  return 0;
}

static int read_frame_len(int fd, uint32_t* len) {
  char b4[4];
  int err = read_full(fd, b4, 4);
  if (err != 0) return err;
  *len = static_cast<uint32_t>(static_cast<uint8_t>(b4[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b4[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b4[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b4[3])) << 24);
  return 0;
}

static void teardown_connection() {
  if (g_fd >= 0) {
    fprintf(stderr, "[go-bridge] teardown fd=%d connected=%d\n", g_fd,
            g_connected);
#ifdef _WIN32
    _close(g_fd);
#else
    shutdown(g_fd, SHUT_RDWR);
    close(g_fd);
#endif
    g_fd = -1;
  }
  g_connected = 0;
}

#ifdef _WIN32
// Named-pipe dial: CreateFileA, then expose the handle as a CRT fd so
// every frame helper above stays identical across platforms.
static int dial_bridge_endpoint(const char* endpoint) {
  HANDLE h = CreateFileA(endpoint, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_BINARY);
  if (fd < 0) CloseHandle(h);
  return fd;
}
#else
static int dial_bridge_endpoint(const char* endpoint) {
  int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, endpoint, sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {BRIDGE_TIMEOUT_SEC, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int bufsz = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  return fd;
}
#endif

static bool Connect(const char* endpoint, const char* token) {
  int fd = dial_bridge_endpoint(endpoint);
  if (fd < 0) {
    ThrowE(g_env, "go-bridge: connect failed");
    return false;
  }
  char hello[192];
  int hn = snprintf(hello, sizeof(hello), "{\"hello\":1,\"token\":\"%s\"}\n",
                    token);
  uint8_t hlen4[4] = {static_cast<uint8_t>(hn),
                      static_cast<uint8_t>(hn >> 8),
                      static_cast<uint8_t>(hn >> 16),
                      static_cast<uint8_t>(hn >> 24)};
  if (write_all(fd, reinterpret_cast<char*>(hlen4), 4) != 0 ||
      write_all(fd, hello, static_cast<size_t>(hn)) != 0) {
    close(fd);
    ThrowE(g_env, "go-bridge: hello write failed");
    return false;
  }
  uint32_t rlen = 0;
  int err = read_frame_len(fd, &rlen);
  if (err == 0 && (rlen == 0 || rlen > 1024 * 1024)) err = EPROTO;
  char ready[256] = {0};
  if (err == 0 && rlen < sizeof(ready)) {
    err = read_full(fd, ready, rlen);
  }
  if (err != 0 || strstr(ready, "\"ready\"") == nullptr) {
    if (err != 0) {
      fprintf(stderr, "[go-bridge] handshake failed (read): %s (rlen=%u)\n",
              strerror(err), rlen);
    } else {
      fprintf(stderr, "[go-bridge] ready got: %.80s\n", ready);
    }
    close(fd);
    ThrowE(g_env, "go-bridge: handshake failed");
    return false;
  }
  g_fd = fd;
  g_connected = 1;
  return true;
}

// One synchronous v3 frame round trip. The request bytes are written
// directly from the caller's buffer (the serialized Buffer stays alive on
// the JS stack for the duration); the response lands in a heap vector.
static int frame_roundtrip(const uint8_t* req, size_t req_len,
                           std::vector<uint8_t>* resp) {
  pthread_mutex_lock(&g_io_mu);
  if (!g_connected) {
    pthread_mutex_unlock(&g_io_mu);
    return ENOTCONN;
  }
  uint8_t len4[4] = {static_cast<uint8_t>(req_len),
                     static_cast<uint8_t>(req_len >> 8),
                     static_cast<uint8_t>(req_len >> 16),
                     static_cast<uint8_t>(req_len >> 24)};
  int werr = write_all(g_fd, reinterpret_cast<char*>(len4), 4);
  if (werr == 0) {
    werr = write_all(g_fd, reinterpret_cast<const char*>(req), req_len);
  }
  uint32_t rlen = 0;
  int rerr = werr;
  if (rerr == 0) rerr = read_frame_len(g_fd, &rlen);
  if (rerr == 0 && rlen > 256u * 1024u * 1024u) rerr = EFBIG;
  if (rerr == 0) {
    resp->resize(rlen);
    rerr = read_full(g_fd, reinterpret_cast<char*>(resp->data()), rlen);
  }
  if (rerr != 0) {
    teardown_connection();
  }
  pthread_mutex_unlock(&g_io_mu);
  return rerr;
}

// ----------------------------------------------------------------------
// fast wire serde — C-side ValueSerializer-format writer/reader that
// removes the injected-JS serde call boundaries (the measured delta vs
// the v8-direct addon). The wire subset mirrors internal/v8serde, which
// is byte-validated against node's v8.serialize: objects, dense arrays,
// one-byte/two-byte strings (with V8's even-alignment 0x00 padding
// before two-byte contents), int32 (zigzag varint) / uint32 (varint) /
// double numbers, booleans, null, undefined, and typed arrays in node's
// host-object form (0x5C + type 1 + byte length + raw bytes). Any shape
// outside the subset makes the walker bail (return false) and the
// caller falls back to the injected JS serde — wire output is always
// compatible either way.

struct SerBuf {
  std::vector<uint8_t> b;
};

static void sb_varint(SerBuf* s, uint64_t v) {
  while (v >= 0x80) {
    s->b.push_back(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  s->b.push_back(static_cast<uint8_t>(v));
}

static size_t sb_varint_len(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

static void sb_zigzag(SerBuf* s, int32_t v) {
  uint32_t z = (static_cast<uint32_t>(v) << 1) ^ (v >> 31);
  sb_varint(s, z);
}

static void sb_utf16_to_utf8(const uint16_t* u, size_t n, std::string* out) {
  for (size_t i = 0; i < n; i++) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
      uint32_t lo = u[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    if (c < 0x80) {
      out->push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (c >> 6)));
      out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (c >> 12)));
      out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (c >> 18)));
      out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
}

// one-byte strings on the wire are latin1 (code units ≤ 0xFF written as
// raw bytes); two-byte strings are UTF-16LE with the contents aligned to
// an even buffer offset via a 0x00 padding byte before the tag.
static void sb_string(SerBuf* s, const uint16_t* u, size_t n) {
  bool one = true;
  for (size_t i = 0; i < n; i++) {
    if (u[i] > 0xFF) {
      one = false;
      break;
    }
  }
  if (one) {
    s->b.push_back(0x22);
    sb_varint(s, n);
    for (size_t i = 0; i < n; i++) s->b.push_back(static_cast<uint8_t>(u[i]));
    return;
  }
  size_t payload = n * 2;
  if ((s->b.size() + 1 + sb_varint_len(payload)) & 1) {
    s->b.push_back(0x00);  // kPadding
  }
  s->b.push_back(0x63);
  sb_varint(s, payload);
  for (size_t i = 0; i < n; i++) {
    s->b.push_back(static_cast<uint8_t>(u[i] & 0xFF));
    s->b.push_back(static_cast<uint8_t>(u[i] >> 8));
  }
}

static bool fast_ser_value(napi_env env, napi_value v, SerBuf* s, int depth) {
  if (depth > 64) return false;
  napi_valuetype t = napi_undefined;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  switch (t) {
    case napi_undefined:
      s->b.push_back(0x5F);
      return true;
    case napi_null:
      s->b.push_back(0x30);
      return true;
    case napi_boolean: {
      bool bv = false;
      if (napi_get_value_bool(env, v, &bv) != napi_ok) return false;
      s->b.push_back(bv ? 0x54 : 0x46);
      return true;
    }
    case napi_number: {
      double d = 0;
      if (napi_get_value_double(env, v, &d) != napi_ok) return false;
      if (d == (double)(int32_t)d && d == d && d * 0 == 0) {
        // integral, finite, fits int32 — node picks the Int32 tag here
        s->b.push_back(0x49);
        sb_zigzag(s, (int32_t)d);
      } else {
        s->b.push_back(0x4E);
        uint64_t bits;
        memcpy(&bits, &d, 8);
        for (int i = 0; i < 8; i++) s->b.push_back((uint8_t)(bits >> (8 * i)));
      }
      return true;
    }
    case napi_string: {
      // utf8 first: when the utf8 length equals the code-unit count the
      // string is pure ASCII and its utf8 bytes ARE the one-byte wire
      // form — no UTF-16 materialization. Non-ASCII falls to the utf16
      // view to distinguish latin1 from two-byte.
      size_t n8 = 0, n16 = 0;
      if (napi_get_value_string_utf8(env, v, nullptr, 0, &n8) != napi_ok ||
          napi_get_value_string_utf16(env, v, nullptr, 0, &n16) != napi_ok) {
        return false;
      }
      if (n8 == n16) {
        // pure ASCII: write straight into the wire buffer tail via the
        // latin1 accessor — a plain memcpy of V8's one-byte chars, no
        // utf8 transcode (what the v8-direct serializer does too).
        s->b.push_back(0x22);
        sb_varint(s, n8);
        size_t tail = s->b.size();
        s->b.resize(tail + n8 + 1);  // +1: V8 null-terminates
        if (n8 > 0 &&
            napi_get_value_string_latin1(
                env, v, reinterpret_cast<char*>(&s->b[tail]), n8 + 1,
                &n8) != napi_ok) {
          return false;
        }
        s->b.resize(tail + n8);
        return true;
      }
      // +1 unit: V8 null-terminates within the given capacity
      std::vector<uint16_t> u(n16 + 1);
      if (n16 > 0 && napi_get_value_string_utf16(
                         env, v, reinterpret_cast<char16_t*>(u.data()), n16 + 1,
                         &n16) != napi_ok) {
        return false;
      }
      sb_string(s, u.data(), n16);
      return true;
    }
    case napi_object:
      break;
    default:
      return false;  // symbol, external, bigint, function
  }
  bool is_ta = false, is_arr = false;
  napi_is_typedarray(env, v, &is_ta);
  napi_is_array(env, v, &is_arr);
  if (is_ta) {
    napi_typedarray_type tt = (napi_typedarray_type)0;
    size_t len = 0, off = 0;
    void* data = nullptr;
    napi_value ab = nullptr;
    if (napi_get_typedarray_info(env, v, &tt, &len, &data, &ab, &off) !=
        napi_ok) {
      return false;
    }
    if (data == nullptr && len > 0) return false;
    // node's host-object type ids: Buffer = 10, other typed arrays = 1.
    // napi_is_buffer is unreliable across node bands (returns true for
    // plain Uint8Arrays on node 25) — identify via the constructor name.
    napi_value ctor = GetProp(env, v, "constructor");
    napi_value nm = ctor ? GetProp(env, ctor, "name") : nullptr;
    std::string cname;
    if (nm) U8(env, nm, &cname);
    s->b.push_back(0x5C);
    sb_varint(s, cname == "Buffer" ? 10 : 1);
    sb_varint(s, len);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    s->b.insert(s->b.end(), p, p + len);
    return true;
  }
  if (is_arr) {
    uint32_t n = 0;
    if (napi_get_array_length(env, v, &n) != napi_ok) return false;
    s->b.push_back(0x41);
    sb_varint(s, n);
    for (uint32_t i = 0; i < n; i++) {
      napi_value el = nullptr;
      if (napi_get_element(env, v, i, &el) != napi_ok) return false;
      if (!fast_ser_value(env, el, s, depth + 1)) return false;
    }
    s->b.push_back(0x24);
    sb_varint(s, 0);
    sb_varint(s, n);
    return true;
  }
  // plain object only beyond here. Exotics (Date, ArrayBuffer, Map, Set,
  // class instances) take the JS-serde fallback: their prototype must be
  // exactly Object.prototype (cached once), and dates/arraybuffers are
  // rejected by direct probe first.
  bool is_date = false, is_ab = false;
  napi_is_date(env, v, &is_date);
  napi_is_arraybuffer(env, v, &is_ab);
  if (is_date || is_ab) return false;
  napi_value proto = nullptr;
  if (napi_get_prototype(env, v, &proto) != napi_ok || !proto) return false;
  napi_value objproto = RefV(g_objproto);
  if (!objproto) return false;
  bool same = false;
  if (napi_strict_equals(env, proto, objproto, &same) != napi_ok || !same) {
    return false;
  }
  napi_value names = nullptr;
  if (napi_get_property_names(env, v, &names) != napi_ok || !names) {
    return false;
  }
  uint32_t n = 0;
  if (napi_get_array_length(env, names, &n) != napi_ok) return false;
  s->b.push_back(0x6F);
  uint32_t written = 0;
  for (uint32_t i = 0; i < n; i++) {
    napi_value k = nullptr, val = nullptr;
    if (napi_get_element(env, names, i, &k) != napi_ok || !k) return false;
    napi_valuetype kt = napi_undefined;
    napi_typeof(env, k, &kt);
    if (kt != napi_string) return false;  // symbol keys etc. → JS serde
    if (napi_get_property(env, v, k, &val) != napi_ok) return false;
    napi_valuetype vt2 = napi_undefined;
    napi_typeof(env, val, &vt2);
    if (vt2 == napi_undefined) {
      continue;  // V8 drops undefined-valued properties
    }
    if (!fast_ser_value(env, k, s, depth + 1)) return false;
    if (!fast_ser_value(env, val, s, depth + 1)) return false;
    written++;
  }
  s->b.push_back(0x7B);
  sb_varint(s, written);
  return true;
}

static bool fast_deser_value(napi_env env, const uint8_t** p, const uint8_t* end,
                             napi_value* out, int depth) {
  if (depth > 64) return false;
  uint8_t tag = 0;
  for (;;) {
    if (*p >= end) return false;
    tag = *(*p)++;
    if (tag != 0x00) break;  // kPadding
  }
  switch (tag) {
    case 0x30:
    case 0x5F:
      napi_get_null(env, out);
      return true;
    case 0x54:
      napi_get_boolean(env, true, out);
      return true;
    case 0x46:
      napi_get_boolean(env, false, out);
      return true;
    case 0x49:
    case 0x55: {
      uint64_t v = 0;
      int shift = 0;
      bool neg = tag == 0x49;
      for (;;) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (b < 0x80) break;
        shift += 7;
        if (shift > 63) return false;
      }
      double d;
      if (neg) {
        int32_t z = (int32_t)((uint32_t)v >> 1) ^ -(int32_t)(v & 1);
        d = (double)z;
      } else {
        d = (double)v;
      }
      return napi_create_double(env, d, out) == napi_ok;
    }
    case 0x4E: {
      if (end - *p < 8) return false;
      uint64_t bits = 0;
      for (int i = 0; i < 8; i++) bits |= (uint64_t)(*p)[i] << (8 * i);
      *p += 8;
      double d;
      memcpy(&d, &bits, 8);
      return napi_create_double(env, d, out) == napi_ok;
    }
    case 0x22:
    case 0x63: {
      uint64_t n = 0;
      int shift = 0;
      for (;;) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        n |= (uint64_t)(b & 0x7F) << shift;
        if (b < 0x80) break;
        shift += 7;
        if (shift > 63) return false;
      }
      if ((uint64_t)(end - *p) < n) return false;
      if (tag == 0x22) {
        // the one-byte contents ARE latin1 — V8 materializes its
        // one-byte string natively (v8-direct deserializer parity)
        bool ok = napi_create_string_latin1(
                      env, reinterpret_cast<const char*>(*p), (size_t)n,
                      out) == napi_ok;
        *p += n;
        return ok;
      }
      if (n & 1) return false;
      std::string utf8;
      sb_utf16_to_utf8(reinterpret_cast<const uint16_t*>(*p), n / 2, &utf8);
      *p += n;
      return napi_create_string_utf8(env, utf8.data(), utf8.size(), out) ==
             napi_ok;
    }
    case 0x6F: {
      napi_value obj = nullptr;
      if (napi_create_object(env, &obj) != napi_ok) return false;
      for (;;) {
        if (*p >= end) return false;
        if (**p == 0x7B) {
          (*p)++;
          // property count varint
          for (;;) {
            if (*p >= end) return false;
            uint8_t b = *(*p)++;
            if (b < 0x80) break;
          }
          break;
        }
        napi_value k = nullptr, val = nullptr;
        if (!fast_deser_value(env, p, end, &k, depth + 1)) return false;
        std::string key;
        if (!U8(env, k, &key)) return false;
        if (!fast_deser_value(env, p, end, &val, depth + 1)) return false;
        if (napi_set_named_property(env, obj, key.c_str(), val) != napi_ok) {
          return false;
        }
      }
      *out = obj;
      return true;
    }
    case 0x41: {
      uint64_t n = 0;
      int shift = 0;
      for (;;) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        n |= (uint64_t)(b & 0x7F) << shift;
        if (b < 0x80) break;
        shift += 7;
        if (shift > 63) return false;
      }
      if (n > (uint64_t)1e7) return false;
      napi_value arr = nullptr;
      if (napi_create_array_with_length(env, (size_t)n, &arr) != napi_ok) {
        return false;
      }
      for (uint64_t i = 0; i < n; i++) {
        napi_value el = nullptr;
        if (!fast_deser_value(env, p, end, &el, depth + 1)) return false;
        if (napi_set_element(env, arr, (uint32_t)i, el) != napi_ok) {
          return false;
        }
      }
      if (*p >= end || *(*p)++ != 0x24) return false;
      for (int k = 0; k < 2; k++) {  // level + element-count varints
        for (;;) {
          if (*p >= end) return false;
          uint8_t b = *(*p)++;
          if (b < 0x80) break;
        }
      }
      *out = arr;
      return true;
    }
    case 0x5C: {
      // 0x5C varint(type=1) varint(byteLen) raw bytes → Buffer
      for (;;) {  // type id varint
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        if (b < 0x80) break;
      }
      uint64_t n = 0;
      int shift = 0;
      for (;;) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        n |= (uint64_t)(b & 0x7F) << shift;
        if (b < 0x80) break;
        shift += 7;
        if (shift > 63) return false;
      }
      if ((uint64_t)(end - *p) < n || n > (uint64_t)256 * 1024 * 1024) {
        return false;
      }
      void* data = nullptr;
      napi_value buf = nullptr;
      if (napi_create_buffer(env, (size_t)n, &data, &buf) != napi_ok) {
        return false;
      }
      memcpy(data, *p, (size_t)n);
      *p += n;
      *out = buf;
      return true;
    }
    default:
      return false;
  }
}

// Bisect gate: unset = both fast paths, "0" = off, "ser" = serialize
// fast only, "des" = deserialize fast only.
static int fast_mode() {
  static int mode = -2;
  if (mode == -2) {
    const char* v = getenv("WAILS_ELECTRON_FASTSERDE");
    if (!v || !*v) mode = 3;
    else if (!strcmp(v, "0")) mode = 0;
    else if (!strcmp(v, "ser")) mode = 1;
    else if (!strcmp(v, "des")) mode = 2;
    else mode = 3;
  }
  return mode;
}

// Fast-path serialize: fills own->b; returns false when the value is
// outside the subset (caller falls back to the injected JS serde).
static bool fast_serialize(napi_value value, SerBuf* s) {
  if (!(fast_mode() & 1)) return false;
  s->b.clear();
  s->b.push_back(0xFF);
  s->b.push_back(0x0F);  // version 15, what node writes on this band
  return fast_ser_value(g_env, value, s, 0);
}

// Fast-path deserialize over a complete frame; returns false when the
// bytes contain anything outside the subset (caller falls back to the
// injected JS v8.deserialize).
static bool fast_deserialize(napi_env env, const uint8_t* data, size_t len,
                             napi_value* out) {
  if (!(fast_mode() & 2)) return false;
  if (len < 2 || data[0] != 0xFF) return false;
  const uint8_t* p = data + 1;
  const uint8_t* end = data + len;
  for (;;) {  // version varint
    if (p >= end) return false;
    uint8_t b = *p++;
    if (b < 0x80) break;
  }
  if (!fast_deser_value(env, &p, end, out, 0)) return false;
  return true;  // trailing bytes tolerated
}

// ----------------------------------------------------------------------
// serde via the injected node v8 module

// Serialize any JS value to wire bytes. Fast C path first (no JS
// boundary, no intermediate copy — v8-direct parity); values outside the
// wire subset fall back to the injected v8.serialize (native
// ValueSerializer — the same bytes the Go decoder already speaks).
// Returns the raw pointer into the produced bytes. The fast path writes
// into a thread-local buffer whose capacity persists across calls (the
// fd write completes before the next serialize on this thread); the JS
// path's lifetime is *buf_out on the caller's stack.
static bool serialize_value(napi_value value, const uint8_t** out,
                            size_t* out_len, napi_value* buf_out) {
  if (g_env) {
    static thread_local SerBuf s;
    if (fast_serialize(value, &s)) {
      *out = s.b.data();
      *out_len = s.b.size();
      *buf_out = nullptr;
      return true;
    }
    bool pending = false;
    napi_is_exception_pending(g_env, &pending);
    if (pending) return false;  // exotic walker raised — propagate
  }
  napi_value ser = RefV(g_v8ser);
  napi_value recv = RefV(g_v8mod);
  if (!ser || !recv) {
    ThrowE(g_env, "go-bridge: serde not injected (preloadInit(electron, v8))");
    return false;
  }
  napi_value buf = CallFn(ser, recv, 1, &value);
  if (!buf) return false;  // exception pending (e.g. DataCloneError)
  // The result is a Buffer (Uint8Array) — pull its raw span.
  void* data = nullptr;
  size_t len = 0;
  napi_typedarray_type t = (napi_typedarray_type)0;
  size_t off = 0;
  napi_value ab = nullptr;
  if (napi_get_typedarray_info(g_env, buf, &t, &len, &data, &ab, &off) ==
          napi_ok &&
      data != nullptr) {
    *out = static_cast<const uint8_t*>(data);
    *out_len = len;
    *buf_out = buf;
    return true;
  }
  char* bdata = nullptr;
  if (napi_get_buffer_info(g_env, buf, (void**)&bdata, &len) == napi_ok &&
      bdata != nullptr) {
    *out = reinterpret_cast<const uint8_t*>(bdata);
    *out_len = len;
    *buf_out = buf;
    return true;
  }
  ThrowE(g_env, "go-bridge: serialize returned non-buffer");
  return false;
}

// Deserialize wire bytes. Fast C path first (subset decode, one copy for
// the Uint8Array — the same materialization v8-direct makes); anything
// outside the subset falls back to the injected v8.deserialize. Zero
// copy there when an external ArrayBuffer is permitted (the finalizer
// owns and frees the heap vector); otherwise one copy into a fresh
// Buffer. Takes ownership of *resp either way. Returns nullptr with an
// exception pending on failure.
static napi_value deserialize_bytes(std::vector<uint8_t>* resp) {
  napi_value fast = nullptr;
  if (g_env && fast_deserialize(g_env, resp->data(), resp->size(), &fast)) {
    delete resp;
    return fast;
  }
  bool pending = false;
  if (g_env) napi_is_exception_pending(g_env, &pending);
  if (pending) {
    delete resp;
    return nullptr;
  }
  napi_value des = RefV(g_v8des);
  napi_value recv = RefV(g_v8mod);
  if (!des || !recv) {
    ThrowE(g_env, "go-bridge: serde not injected (preloadInit(electron, v8))");
    delete resp;
    return nullptr;
  }
  napi_value input = nullptr;
  napi_value ext = nullptr;
  if (napi_create_external_arraybuffer(
          g_env, resp->data(), resp->size(),
          [](napi_env /*env*/, void* /*data*/, void* hint) {
            delete static_cast<std::vector<uint8_t>*>(hint);
          },
          resp, &ext) == napi_ok &&
      napi_create_typedarray(g_env, napi_uint8_array, resp->size(), ext, 0,
                             &input) == napi_ok) {
    resp = nullptr;  // ownership moved to the finalizer
  } else {
    // sandboxed runtimes may refuse external backing stores — copy then
    void* copied = nullptr;
    if (napi_create_buffer(g_env, resp->size(), &copied, &input) != napi_ok) {
      ThrowE(g_env, "go-bridge: response buffer alloc failed");
      delete resp;
      return nullptr;
    }
    memcpy(copied, resp->data(), resp->size());
    delete resp;
  }
  return CallFn(des, recv, 1, &input);
}

// ----------------------------------------------------------------------
// exports

// preloadInit(electron, v8): the preload surface that does not need the
// connection yet — postMessage shim, file-drop contract, late-init hook.
static napi_value PreloadInit(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  g_env = env;
  if (argc < 1) {
    ThrowE(env, "preloadInit(electron, v8) requires the electron module");
    return nullptr;
  }
  napi_value electron = argv[0];
  napi_value ipc = GetProp(env, electron, "ipcRenderer");
  if (ipc) napi_create_reference(env, ipc, 1, &g_ipc_renderer);
  napi_value wu = GetProp(env, electron, "webUtils");
  if (wu) napi_create_reference(env, wu, 1, &g_webutils);
  if (argc >= 2 && argv[1]) {
    napi_valuetype vt = napi_undefined;
    napi_typeof(env, argv[1], &vt);
    if (vt == napi_object || vt == napi_function) {
      napi_create_reference(env, argv[1], 1, &g_v8mod);
      napi_value ser = GetProp(env, argv[1], "serialize");
      napi_value des = GetProp(env, argv[1], "deserialize");
      if (ser) napi_create_reference(env, ser, 1, &g_v8ser);
      if (des) napi_create_reference(env, des, 1, &g_v8des);
    }
  }
  { // cache Object.prototype for the fast-serde exotic gate
    napi_value global = nullptr, objctor = nullptr;
    napi_get_global(env, &global);
    napi_value obj = GetProp(env, global, "Object");
    if (obj) objctor = GetProp(env, obj, "prototype");
    if (objctor) napi_create_reference(env, objctor, 1, &g_objproto);
  }

  napi_value global = nullptr;
  napi_get_global(env, &global);

  // window.chrome = window.chrome || {}; chrome.webview.postMessage shim
  napi_value chrome = GetProp(env, global, "chrome");
  napi_valuetype ct = napi_undefined;
  if (chrome) napi_typeof(env, chrome, &ct);
  if (ct != napi_object) {
    napi_create_object(env, &chrome);
    SetProp(env, global, "chrome", chrome);
  }
  napi_value webview = nullptr;
  napi_create_object(env, &webview);
  SetProp(env, chrome, "webview", webview);

  // postMessage shim: ipcRenderer.send('wails:message', msg)
  napi_value shim = nullptr;
  napi_create_function(
      env, "postMessage", NAPI_AUTO_LENGTH,
      [](napi_env e, napi_callback_info i) -> napi_value {
        size_t c = 1;
        napi_value a[1];
        napi_get_cb_info(e, i, &c, a, nullptr, nullptr);
        napi_value ipc2 = RefV(g_ipc_renderer);
        if (!ipc2 || c < 1) return nullptr;
        napi_value send = GetProp(e, ipc2, "send");
        napi_value sargv[2] = {NF(e, "wails:message"), a[0]};
        CallFn(send, ipc2, 2, sargv);
        return nullptr;
      },
      nullptr, &shim);
  SetProp(env, webview, "postMessage", shim);

  napi_value pma = nullptr;
  napi_create_function(env, "postMessageWithAdditionalObjects",
                       NAPI_AUTO_LENGTH, PostMessageAdditionalCb, nullptr,
                       &pma);
  SetProp(env, webview, "postMessageWithAdditionalObjects", pma);

  napi_value init = nullptr;
  napi_create_function(env, "__wailsNativeInit", NAPI_AUTO_LENGTH,
                       NativeInitThunk, nullptr, &init);
  SetProp(env, global, "__wailsNativeInit", init);
  return nullptr;
}

// postMessageWithAdditionalObjects("file:drop:<x>:<y>", files): the
// WebView2 file-drop contract; resolves real paths via webUtils and
// forwards wails:file-drop:<json> over the ipc channel.
static napi_value PostMessageAdditionalCb(napi_env env,
                                          napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return nullptr;
  napi_valuetype t0 = napi_undefined;
  bool isarr = false;
  napi_typeof(env, argv[0], &t0);
  napi_is_array(env, argv[1], &isarr);
  if (t0 != napi_string || !isarr) return nullptr;
  std::string msg;
  if (!U8(env, argv[0], &msg) || msg.compare(0, 10, "file:drop:") != 0) {
    return nullptr;
  }
  int x = 0, y = 0;
  sscanf(msg.c_str() + 10, "%d:%d", &x, &y);

  napi_value webutils = RefV(g_webutils);
  napi_value getpath =
      webutils ? GetProp(env, webutils, "getPathForFile") : nullptr;

  uint32_t n = 0;
  napi_get_array_length(env, argv[1], &n);
  napi_value names = nullptr;
  napi_create_array_with_length(env, n, &names);
  uint32_t out = 0;
  for (uint32_t i = 0; i < n; i++) {
    napi_value filev = nullptr;
    napi_get_element(env, argv[1], i, &filev);
    if (!filev || !getpath) continue;
    napi_value path = CallFn(getpath, webutils, 1, &filev);
    std::string p;
    if (path && U8(env, path, &p) && !p.empty()) {
      napi_set_element(env, names, out++, NF(env, p.c_str()));
    }
  }
  napi_value payload = nullptr;
  napi_create_object(env, &payload);
  napi_value xv = nullptr, yv = nullptr;
  napi_create_int32(env, x, &xv);
  napi_create_int32(env, y, &yv);
  SetProp(env, payload, "x", xv);
  SetProp(env, payload, "y", yv);
  SetProp(env, payload, "filenames", names);

  // JSON.stringify(payload)
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_value jsonobj = GetProp(env, global, "JSON");
  napi_value stringify = jsonobj ? GetProp(env, jsonobj, "stringify") : nullptr;
  napi_value json =
      stringify ? CallFn(stringify, jsonobj, 1, &payload) : nullptr;
  std::string js;
  if (!json || !U8(env, json, &js)) return nullptr;

  std::string full = "wails:file-drop:" + js;
  napi_value ipc = RefV(g_ipc_renderer);
  if (!ipc) return nullptr;
  napi_value send = GetProp(env, ipc, "send");
  napi_value sargv[2] = {NF(env, "wails:message"), NF(env, full.c_str())};
  CallFn(send, ipc, 2, sargv);
  return nullptr;
}

// __wailsNativeInit({endpoint, token, addon}) — connect + flags + fetch
// override. Same entry the control plane injects via executeJavaScript.
static napi_value NativeInitThunk(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  g_env = env;
  if (argc < 1) {
    ThrowE(env, "__wailsNativeInit(config) requires an object");
    return nullptr;
  }
  std::string endpoint, token;
  U8(env, GetProp(env, argv[0], "endpoint"), &endpoint);
  U8(env, GetProp(env, argv[0], "token"), &token);
  if (endpoint.empty() || token.empty()) {
    ThrowE(env, "go-bridge: config missing endpoint/token");
    return nullptr;
  }
  if (!Connect(endpoint.c_str(), token.c_str())) {
    return nullptr;  // exception already pending
  }

  napi_value global = nullptr;
  napi_get_global(env, &global);
  // resolve the Response constructor once
  napi_value response = GetProp(env, global, "Response");
  napi_valuetype rt = napi_undefined;
  if (response) napi_typeof(env, response, &rt);
  if (rt == napi_function) {
    napi_create_reference(env, response, 1, &g_response_ctor);
  }
  // runtime.js gates its call-body encoding on these (set only after the
  // handshake, so a failed connect leaves the plain-HTTP fallback intact)
  napi_value tru = nullptr;
  napi_get_boolean(env, true, &tru);
  SetProp(env, global, "__wailsV8Body", tru);
  // node's v8.serialize/deserialize ARE the wire serde — expose directly
  napi_value ser = RefV(g_v8ser);
  napi_value des = RefV(g_v8des);
  if (ser) SetProp(env, global, "__wailsV8Serialize", ser);
  if (des) SetProp(env, global, "__wailsV8Deserialize", des);

  napi_value inv = nullptr, echo = nullptr;
  napi_create_function(env, "__nativeInvoke", NAPI_AUTO_LENGTH, NativeInvoke,
                       nullptr, &inv);
  napi_create_function(env, "__nativeEcho", NAPI_AUTO_LENGTH, NativeEcho,
                       nullptr, &echo);
  SetProp(env, global, "__nativeInvoke", inv);
  SetProp(env, global, "__nativeEcho", echo);
  SetProp(env, global, "__nativeCall", echo);  // legacy existence gate

  // install the fetch override, keeping the original
  napi_value orig = GetProp(env, global, "fetch");
  if (orig) napi_create_reference(env, orig, 1, &g_orig_fetch);
  napi_value fo = nullptr;
  napi_create_function(env, "fetch", NAPI_AUTO_LENGTH, FetchOverride, nullptr,
                       &fo);
  SetProp(env, global, "fetch", fo);
  SetProp(env, global, "__nativeHttpActive", tru);

  fprintf(stderr, "[go-bridge] native transport ready (napi)\n");
  return nullptr;
}

// FetchOverride(input, init): replaces window.fetch. Routes /wails/runtime
// requests over the bridge data plane; everything else goes to the
// original fetch. Synchronous body — the caller's await tolerates a plain
// Response return, exactly like the v8-direct addon.
static napi_value FetchOverride(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // fast bail: URL must name the runtime path
  bool native = g_connected != 0;
  static const char kMarker[] = "/wails/runtime";
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  napi_value url_v = undef;
  if (native && argc >= 1) {
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t == napi_string) {
      url_v = argv[0];
    } else if (t == napi_object) {
      // Request objects carry .url, URL objects .href — String(input)
      // semantics like the JS preload had
      napi_value u = GetProp(env, argv[0], "url");
      napi_valuetype ut = napi_undefined;
      if (u) napi_typeof(env, u, &ut);
      if (ut != napi_string) {
        u = GetProp(env, argv[0], "href");
        if (u) napi_typeof(env, u, &ut);
      }
      if (ut == napi_string) {
        url_v = u;
      } else {
        napi_coerce_to_string(env, argv[0], &url_v);
      }
    }
  }
  std::string url;
  U8(env, url_v, &url);
  if (!native || url.find(kMarker) == std::string::npos) {
    napi_value orig = RefV(g_orig_fetch);
    if (!orig) {
      ThrowE(env, "go-bridge: fetch override not initialized");
      return nullptr;
    }
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value out = nullptr;
    if (argc >= 2) {
      napi_value a2[2] = {argv[0], argv[1]};
      napi_call_function(env, global, orig, 2, a2, &out);
    } else if (argc == 1) {
      napi_call_function(env, global, orig, 1, argv, &out);
    }
    return out;
  }

  // extract init fields (all references — zero conversion)
  napi_value method = undef, headers = undef, body = undef;
  if (argc >= 2 && argv[1]) {
    napi_valuetype it = napi_undefined;
    napi_typeof(env, argv[1], &it);
    if (it == napi_object) {
      method = GetProp(env, argv[1], "method");
      headers = GetProp(env, argv[1], "headers");
      body = GetProp(env, argv[1], "body");
    }
  }
  std::string method_s;
  if (!U8(env, method, &method_s) || method_s.empty()) method_s = "GET";

  // build the call object — plain JS references, then one native write
  napi_value call = nullptr;
  napi_create_object(env, &call);
  SetProp(env, call, "channel", NF(env, "http"));
  SetProp(env, call, "method", NF(env, method_s.c_str()));
  SetProp(env, call, "url", url_v);
  napi_valuetype bt = napi_undefined;
  if (body) napi_typeof(env, body, &bt);
  SetProp(env, call, "body", bt == napi_undefined ? NF(env, "") : body);
  napi_valuetype ht = napi_undefined;
  if (headers) napi_typeof(env, headers, &ht);
  if (ht == napi_object) {
    SetProp(env, call, "headers", headers);
  } else {
    napi_value eh = nullptr;
    napi_create_object(env, &eh);
    SetProp(env, call, "headers", eh);
  }

  const uint8_t* req = nullptr;
  size_t req_len = 0;
  napi_value req_buf = nullptr;
  if (!serialize_value(call, &req, &req_len, &req_buf)) {
    fprintf(stderr, "[go-bridge] fetch: serialize failed\n");
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (!pending) {
      ThrowE(env, "go-bridge: serialize call failed");
    }
    return nullptr;
  }
  std::vector<uint8_t> resp;
  int err = frame_roundtrip(req, req_len, &resp);
  if (err != 0) {
    ThrowE(env,
           std::string("go-bridge: invoke failed (") + strerror(err) + ")");
    return nullptr;
  }

  napi_value out = deserialize_bytes(new std::vector<uint8_t>(std::move(resp)));
  if (!out) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (!pending) {
      ThrowE(env, "go-bridge: bad response frame");
    }
    return nullptr;
  }

  // build the Response: new Response(bodyBuffer, {status, headers})
  napi_value status_v = GetProp(env, out, "status");
  double status = 0;
  napi_get_value_double(env, status_v, &status);
  napi_value ctype = GetProp(env, out, "contentType");
  std::string ctype_s;
  U8(env, ctype, &ctype_s);
  napi_value body_v = GetProp(env, out, "body");

  napi_value resp_init = nullptr;
  napi_create_object(env, &resp_init);
  napi_value status_n = nullptr;
  napi_create_int32(env, (int32_t)status, &status_n);
  SetProp(env, resp_init, "status", status_n);
  napi_value rh = nullptr;
  napi_create_object(env, &rh);
  if (!ctype_s.empty()) {
    SetProp(env, rh, "Content-Type", NF(env, ctype_s.c_str()));
  }
  SetProp(env, resp_init, "headers", rh);

  napi_value cargv[2];
  napi_valuetype bvt = napi_undefined;
  if (body_v) napi_typeof(env, body_v, &bvt);
  bool isview = false;
  if (body_v) napi_is_typedarray(env, body_v, &isview);
  if (isview || bvt == napi_string) {
    cargv[0] = body_v;
  } else {
    napi_get_null(env, &cargv[0]);
  }
  cargv[1] = resp_init;
  napi_value ctor = RefV(g_response_ctor);
  if (!ctor) {
    ThrowE(env, "go-bridge: Response ctor missing");
    return nullptr;
  }
  napi_value response_obj = nullptr;
  if (napi_new_instance(env, ctor, 2, cargv, &response_obj) != napi_ok) {
    return nullptr;  // exception already pending
  }
  return response_obj;
}

// __nativeEcho(payload) — sync {channel:"echo"} round trip, payload back.
static napi_value NativeEcho(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (!g_connected) {
    ThrowE(env, "go-bridge: not connected");
    return nullptr;
  }
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  napi_value msg = nullptr;
  napi_create_object(env, &msg);
  napi_value zero = nullptr;
  napi_create_int32(env, 0, &zero);
  SetProp(env, msg, "id", zero);
  SetProp(env, msg, "channel", NF(env, "echo"));
  SetProp(env, msg, "payload", argc >= 1 ? argv[0] : undef);

  const uint8_t* req = nullptr;
  size_t req_len = 0;
  napi_value req_buf = nullptr;
  if (!serialize_value(msg, &req, &req_len, &req_buf)) {
    ThrowE(env, "go-bridge: serialize echo failed");
    return nullptr;
  }
  std::vector<uint8_t> resp;
  int err = frame_roundtrip(req, req_len, &resp);
  if (err != 0) {
    ThrowE(env, std::string("go-bridge: echo failed (") + strerror(err) + ")");
    return nullptr;
  }
  napi_value out = deserialize_bytes(new std::vector<uint8_t>(std::move(resp)));
  if (!out) {
    ThrowE(env, "go-bridge: bad echo response");
    return nullptr;
  }
  napi_value payload = GetProp(env, out, "payload");
  return payload ? payload : undef;
}

// __nativeInvoke(msgObj) — generic single-frame invoke returning the
// response object (parity with the v8-direct addon's export).
static napi_value NativeInvoke(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (!g_connected) {
    ThrowE(env, "go-bridge: not connected");
    return nullptr;
  }
  napi_valuetype t = napi_undefined;
  if (argc < 1) {
    ThrowE(env, "go-bridge: invoke(msgObj) requires an object");
    return nullptr;
  }
  napi_typeof(env, argv[0], &t);
  if (t != napi_object) {
    ThrowE(env, "go-bridge: invoke(msgObj) requires an object");
    return nullptr;
  }
  const uint8_t* req = nullptr;
  size_t req_len = 0;
  napi_value req_buf = nullptr;
  if (!serialize_value(argv[0], &req, &req_len, &req_buf)) {
    ThrowE(env, "go-bridge: serialize failed");
    return nullptr;
  }
  std::vector<uint8_t> resp;
  int err = frame_roundtrip(req, req_len, &resp);
  if (err != 0) {
    ThrowE(env, std::string("go-bridge: invoke failed (") + strerror(err) + ")");
    return nullptr;
  }
  napi_value out = deserialize_bytes(new std::vector<uint8_t>(std::move(resp)));
  if (!out) {
    ThrowE(env, "go-bridge: bad response frame");
    return nullptr;
  }
  return out;
}

static napi_value Close(napi_env env, napi_callback_info /*info*/) {
  pthread_mutex_lock(&g_io_mu);
  teardown_connection();
  pthread_mutex_unlock(&g_io_mu);
  return nullptr;
}

#ifdef __linux__
// ---- X11 frameless drag/resize via _NET_WM_MOVERESIZE -----------------
// Electron exposes no manual-move API; the window manager conducts the
// interactive move/resize when the app sends this client message (same
// mechanism GTK's begin_move_drag uses). libX11 is dlopen'd so the build
// needs no X11 dev headers; Electron always runs with a DISPLAY on X11.
struct XClientMsgEvent {
  int type;
  unsigned long serial;
  int send_event;
  void* display;
  unsigned long window;
  unsigned long message_type;
  int format;
  unsigned long data[5];
};

static void* g_x11_so = nullptr;
static void* g_xdisp = nullptr;
static unsigned long g_wmmove_atom = 0;
static pthread_mutex_t g_x_mu = PTHREAD_MUTEX_INITIALIZER;

static bool x11_ready() {
  pthread_mutex_lock(&g_x_mu);
  if (g_x11_so == nullptr) {
    g_x11_so = dlopen("libX11.so.6", RTLD_LAZY);
    if (g_x11_so != nullptr) {
      auto init_threads = reinterpret_cast<int (*)(void)>(dlsym(g_x11_so, "XInitThreads"));
      if (init_threads != nullptr) init_threads();
      auto open_disp = reinterpret_cast<void* (*)(const char*)>(dlsym(g_x11_so, "XOpenDisplay"));
      auto intern_atom = reinterpret_cast<unsigned long (*)(void*, const char*, int)>(dlsym(g_x11_so, "XInternAtom"));
      if (open_disp != nullptr && intern_atom != nullptr) {
        g_xdisp = open_disp(nullptr);
        if (g_xdisp != nullptr) {
          g_wmmove_atom = intern_atom(g_xdisp, "_NET_WM_MOVERESIZE", 0);
        }
      }
    }
  }
  bool ok = g_xdisp != nullptr && g_wmmove_atom != 0;
  pthread_mutex_unlock(&g_x_mu);
  return ok;
}

// _NET_WM_MOVERESIZE directions (freedesktop.org wm-spec)
static int x11_direction(const char* edge) {
  if (strcmp(edge, "nw-resize") == 0) return 0;
  if (strcmp(edge, "n-resize") == 0) return 1;
  if (strcmp(edge, "ne-resize") == 0) return 2;
  if (strcmp(edge, "e-resize") == 0) return 3;
  if (strcmp(edge, "se-resize") == 0) return 4;
  if (strcmp(edge, "s-resize") == 0) return 5;
  if (strcmp(edge, "sw-resize") == 0) return 6;
  if (strcmp(edge, "w-resize") == 0) return 7;
  return 8;  // move
}

// The WM-conducted _NET_WM_MOVERESIZE protocol cannot be used while the
// pointer is grabbed by the renderer (Chromium holds an implicit button
// grab for the whole press, and a cross-process XUngrabPointer is not
// possible) — the WM's grab silently fails. Instead we track the pointer
// ourselves and move/resize the window directly, exactly like the CEF
// backend's glue did. Runs synchronously until button release, matching
// native BeginMove semantics.
static void x11_move_resize(unsigned long xid, int direction) {
  if (!x11_ready()) {
    fprintf(stderr, "[go-bridge] x11_move_resize: X11 init failed\n");
    return;
  }
  pthread_mutex_lock(&g_x_mu);
  auto get_geometry = reinterpret_cast<int (*)(void*, unsigned long, unsigned long*, int*, int*, unsigned*, unsigned*, unsigned*, unsigned*)>(dlsym(g_x11_so, "XGetGeometry"));
  auto translate = reinterpret_cast<int (*)(void*, unsigned long, unsigned long, int*, int*)>(dlsym(g_x11_so, "XTranslateCoordinates"));
  auto move_window = reinterpret_cast<int (*)(void*, unsigned long, int, int)>(dlsym(g_x11_so, "XMoveWindow"));
  auto move_resize_window = reinterpret_cast<int (*)(void*, unsigned long, int, int, unsigned, unsigned)>(dlsym(g_x11_so, "XMoveResizeWindow"));
  auto query_pointer = reinterpret_cast<int (*)(void*, unsigned long, unsigned long*, unsigned long*, int*, int*, int*, int*, unsigned*)>(dlsym(g_x11_so, "XQueryPointer"));
  auto flush = reinterpret_cast<int (*)(void*)>(dlsym(g_x11_so, "XFlush"));
  if (get_geometry == nullptr || translate == nullptr || move_window == nullptr ||
      move_resize_window == nullptr || query_pointer == nullptr || flush == nullptr) {
    fprintf(stderr, "[go-bridge] x11_move_resize: missing X11 symbols\n");
    pthread_mutex_unlock(&g_x_mu);
    return;
  }
  unsigned long root_ret = 0;
  int wx = 0, wy = 0;
  unsigned w = 0, h = 0, bw = 0, depth = 0;
  if (get_geometry(g_xdisp, xid, &root_ret, &wx, &wy, &w, &h, &bw, &depth) == 0) {
    fprintf(stderr, "[go-bridge] x11_move_resize: XGetGeometry failed\n");
    pthread_mutex_unlock(&g_x_mu);
    return;
  }
  // window origin -> root coordinates (frameless windows are usually
  // parented to root, but translate anyway for WM-framed cases)
  int rx = wx, ry = wy;
  translate(g_xdisp, xid, root_ret, &rx, &ry);
  unsigned long qroot = 0, qchild = 0;
  int px = 0, py = 0, qwx = 0, qwy = 0;
  unsigned qmask = 0;
  if (!query_pointer(g_xdisp, root_ret, &qroot, &qchild, &px, &py, &qwx, &qwy, &qmask)) {
    pthread_mutex_unlock(&g_x_mu);
    return;
  }
  const int start_px = px, start_py = py;
  const int start_wx = rx, start_wy = ry;
  const unsigned start_w = w, start_h = h;
  const bool resize = direction != 8;
  // direction bits for the arithmetic below
  const bool west = direction == 0 || direction == 6 || direction == 7;   // nw sw w
  const bool north = direction == 0 || direction == 1 || direction == 2;  // nw n ne
  const bool east = direction == 2 || direction == 3 || direction == 4;   // ne e se
  const bool south = direction == 4 || direction == 5 || direction == 6;  // se s sw
  flush(g_xdisp);
  pthread_mutex_unlock(&g_x_mu);

  // track the pointer until the button is released (grab or not,
  // XQueryPointer always reports true root coordinates)
  for (;;) {
    struct timespec ts = {0, 16 * 1000 * 1000};
    nanosleep(&ts, nullptr);
    pthread_mutex_lock(&g_x_mu);
    int nrx = 0, nry = 0;
    unsigned long nr = 0, nc = 0;
    unsigned nmask = 0;
    int nx = 0, ny = 0;
    if (!query_pointer(g_xdisp, root_ret, &nr, &nc, &nrx, &nry, &nx, &ny, &nmask)) {
      pthread_mutex_unlock(&g_x_mu);
      return;
    }
    if ((nmask & (1 << 8)) == 0) {  // Button1 released -> done
      flush(g_xdisp);
      pthread_mutex_unlock(&g_x_mu);
      return;
    }
    if (resize) {
      int nx0 = start_wx, ny0 = start_wy;
      unsigned nw = start_w, nh = start_h;
      if (east) nw = start_w + (nrx - start_px);
      if (south) nh = start_h + (nry - start_py);
      if (west) {
        nx0 = start_wx + (nrx - start_px);
        nw = start_w - (nrx - start_px);
      }
      if (north) {
        ny0 = start_wy + (nry - start_py);
        nh = start_h - (nry - start_py);
      }
      if (nw < 100) { if (west) nx0 = start_wx + (int)start_w - 100; nw = 100; }
      if (nh < 60) { if (north) ny0 = start_wy + (int)start_h - 60; nh = 60; }
      move_resize_window(g_xdisp, xid, nx0, ny0, nw, nh);
    } else {
      move_window(g_xdisp, xid, start_wx + (nrx - start_px), start_wy + (nry - start_py));
    }
    flush(g_xdisp);
    pthread_mutex_unlock(&g_x_mu);
  }
}
#endif // __linux__

static napi_value DispatchMethod(napi_env env, const std::string& m,
                                 napi_value p);

// ======================================================================
// main-process surface: window management, the stdio control protocol,
// and event forwarding — the entire former main.js, native (N-API).
//
//   request : {"t":"req","id":N,"m":"<method>","p":{...,"id":windowID}}
//   response: {"t":"resp","id":N,"ok":true,"r":...} | {"t":"resp",ok:false,err}
//   event   : {"t":"ev","e":"<name>","p":{...,"id":windowID}}
// Quitting: stdin EOF means the host is gone (orphan protection layer 1);
// a PPID poll covers SIGKILL'd hosts (layer 2; Windows uses electron.go's
// job teardown instead).

static napi_ref g_app = nullptr, g_winctor = nullptr, g_ipcmain = nullptr,
                 g_electron = nullptr, g_popup_menu = nullptr;
static napi_ref opts_ref_g = nullptr;
static bool g_m_debug = false;
static std::string g_cfg_preload, g_cfg_bridge_path, g_cfg_bridge_token,
    g_cfg_native_addon;
static uint32_t g_orig_ppid = 0;
static uv_loop_t* g_loop = nullptr;
static uv_async_t g_lines_async;
static uv_async_t g_quit_async;
static std::mutex g_lines_mu;
static std::vector<std::string> g_lines;
static std::mutex g_out_mu;
static std::map<uint32_t, napi_ref> g_windows;
static std::map<int32_t, uint32_t> g_by_wc;

// raw stdout line writer for menu click callbacks (no napi objects
// involved — plain JSON built in place)
void __popup_send_raw(const char* line) {
  std::string l = line;
  l += '\n';
  std::lock_guard<std::mutex> lk(g_out_mu);
  write_all(1, l.data(), l.size());
}

static void ELog(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[wails-electron] ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

// JSON via the engine's global JSON object (never C++ mangling-sensitive
// v8::JSON:: signatures).
static napi_value MJsonParse(napi_env env, napi_value s) {
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_value j = GetProp(env, global, "JSON");
  napi_value parse = j ? GetProp(env, j, "parse") : nullptr;
  return parse ? CallFn(parse, j, 1, &s) : nullptr;
}

static napi_value MJsonStringify(napi_env env, napi_value v) {
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_value j = GetProp(env, global, "JSON");
  napi_value stringify = j ? GetProp(env, j, "stringify") : nullptr;
  return stringify ? CallFn(stringify, j, 1, &v) : nullptr;
}

// JSON object -> stdout line (the control channel to Go).
static void SendObj(napi_env env, napi_value obj) {
  napi_value sv = MJsonStringify(env, obj);
  if (!sv) {
    // stringify raised — dump and clear, or the pending exception poisons
    // every later napi call on this thread
    napi_value exc = nullptr;
    if (napi_get_and_clear_last_exception(env, &exc) == napi_ok && exc) {
      napi_value s = nullptr;
      std::string msg;
      if (napi_coerce_to_string(env, exc, &s) == napi_ok && s) U8(env, s, &msg);
      ELog("SendObj stringify failed: %s", msg.c_str());
    }
    return;
  }
  std::string line;
  if (!sv || !U8(env, sv, &line)) return;
  line += '\n';
  std::lock_guard<std::mutex> lk(g_out_mu);
  if (write_all(1, line.data(), line.size()) != 0 && g_m_debug) {
    ELog("send failed");
  }
}

static napi_value NewEv(napi_env env, const char* ev, uint32_t id) {
  napi_value o = nullptr, p = nullptr;
  napi_create_object(env, &o);
  napi_create_object(env, &p);
  napi_set_named_property(env, o, "t", NF(env, "ev"));
  napi_set_named_property(env, o, "e", NF(env, ev));
  napi_value idv = nullptr;
  napi_create_uint32(env, id, &idv);
  napi_set_named_property(env, p, "id", idv);
  napi_set_named_property(env, o, "p", p);
  return o;
}

static void WinEvent(napi_env env, uint32_t id, const char* ev) {
  SendObj(env, NewEv(env, ev, id));
}

// extra: an object whose own properties are merged into the payload.
static void WinEventExtra(napi_env env, uint32_t id, const char* ev,
                          napi_value extra) {
  napi_value o = NewEv(env, ev, id);
  napi_value p = GetProp(env, o, "p");
  napi_value keys = nullptr;
  if (extra && napi_get_property_names(env, extra, &keys) == napi_ok && keys) {
    uint32_t n = 0;
    napi_get_array_length(env, keys, &n);
    for (uint32_t i = 0; i < n; i++) {
      napi_value k = nullptr, v = nullptr;
      napi_get_element(env, keys, i, &k);
      std::string key;
      if (k && U8(env, k, &key) &&
          napi_get_property(env, extra, k, &v) == napi_ok && v) {
        napi_set_named_property(env, p, key.c_str(), v);
      }
    }
  }
  SendObj(env, o);
}

static void SendResp(napi_env env, int64_t id, napi_value result) {
  napi_value o = nullptr, idv = nullptr, tru = nullptr, r = nullptr;
  napi_create_object(env, &o);
  napi_create_double(env, (double)id, &idv);
  napi_get_boolean(env, true, &tru);
  napi_set_named_property(env, o, "t", NF(env, "resp"));
  napi_set_named_property(env, o, "id", idv);
  napi_set_named_property(env, o, "ok", tru);
  if (result) {
    napi_valuetype rt = napi_undefined;
    napi_typeof(env, result, &rt);
    if (rt == napi_undefined) {
      napi_get_null(env, &r);
    } else {
      r = result;
    }
  } else {
    napi_get_null(env, &r);
  }
  napi_set_named_property(env, o, "r", r);
  SendObj(env, o);
}

static void SendRespErr(napi_env env, int64_t id, const char* err) {
  napi_value o = nullptr, idv = nullptr, fals = nullptr;
  napi_create_object(env, &o);
  napi_create_double(env, (double)id, &idv);
  napi_get_boolean(env, false, &fals);
  napi_set_named_property(env, o, "t", NF(env, "resp"));
  napi_set_named_property(env, o, "id", idv);
  napi_set_named_property(env, o, "ok", fals);
  napi_set_named_property(env, o, "err", NF(env, err));
  SendObj(env, o);
}

// ---- param readers (JSON-shaped, JS-truthiness for booleans) ----
static double MPNum(napi_env env, napi_value p, const char* k, double dflt) {
  napi_value v = p ? GetProp(env, p, k) : nullptr;
  if (!v) return dflt;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, v, &t);
  if (t != napi_number) return dflt;
  double d = dflt;
  napi_get_value_double(env, v, &d);
  return d;
}

static std::string MPStr(napi_env env, napi_value p, const char* k) {
  std::string out;
  napi_value v = p ? GetProp(env, p, k) : nullptr;
  if (v) U8(env, v, &out);
  return out;
}

static bool MPBool(napi_env env, napi_value p, const char* k) {
  napi_value v = p ? GetProp(env, p, k) : nullptr;
  if (!v) return false;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, v, &t);
  if (t == napi_boolean) {
    bool b = false;
    napi_get_value_bool(env, v, &b);
    return b;
  }
  if (t == napi_null || t == napi_undefined) return false;
  if (t == napi_number) {
    double d = 0;
    napi_get_value_double(env, v, &d);
    return d != 0;
  }
  if (t == napi_string) {
    std::string s;
    U8(env, v, &s);
    return !s.empty();
  }
  return true;
}

static napi_value MGetWin(napi_env env, napi_value p) {
  uint32_t id = (uint32_t)MPNum(env, p, "id", -1);
  auto it = g_windows.find(id);
  if (it == g_windows.end()) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Error: unknown window %u", id);
    napi_throw_error(env, nullptr, msg);
    return nullptr;
  }
  return RefV(it->second);
}

// Call a no-arg method on the window (or a sub-object like webContents).
static napi_value MCallWin(napi_env env, napi_value win, const char* obj_key,
                           const char* method) {
  napi_value target = win;
  if (obj_key != nullptr) {
    napi_value t = GetProp(env, win, obj_key);
    napi_valuetype tt = napi_undefined;
    if (!t || napi_typeof(env, t, &tt) != napi_ok || tt != napi_object) {
      return nullptr;
    }
    target = t;
  }
  napi_value fv = target ? GetProp(env, target, method) : nullptr;
  napi_valuetype ft = napi_undefined;
  if (!fv || napi_typeof(env, fv, &ft) != napi_ok || ft != napi_function) {
    napi_throw_error(env, nullptr, "missing method");
    return nullptr;
  }
  return CallFn(fv, target, 0, nullptr);
}

static napi_value MCallWin1(napi_env env, napi_value win, const char* obj_key,
                            const char* method, napi_value a) {
  napi_value target = win;
  if (obj_key != nullptr) {
    napi_value t = GetProp(env, win, obj_key);
    napi_valuetype tt = napi_undefined;
    if (!t || napi_typeof(env, t, &tt) != napi_ok || tt != napi_object) {
      return nullptr;
    }
    target = t;
  }
  napi_value fv = target ? GetProp(env, target, method) : nullptr;
  napi_valuetype ft = napi_undefined;
  if (!fv || napi_typeof(env, fv, &ft) != napi_ok || ft != napi_function) {
    napi_throw_error(env, nullptr, "missing method");
    return nullptr;
  }
  return CallFn(fv, target, 1, &a);
}

// ---- promise-aware respond ----
static napi_value ResolvedCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  g_env = env;
  SendResp(env, *(int64_t*)data, argc >= 1 ? argv[0] : nullptr);
  return nullptr;
}

static napi_value RejectedCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  g_env = env;
  std::string err = "Error";
  if (argc >= 1) {
    napi_coerce_to_string(env, argv[0], &argv[0]);
    U8(env, argv[0], &err);
  }
  SendRespErr(env, *(int64_t*)data, err.c_str());
  return nullptr;
}

static void RespondValue(napi_env env, int64_t id, napi_value result) {
  if (!result) return;  // exception path handled by the caller
  bool is_promise = false;
  napi_is_promise(env, result, &is_promise);
  if (is_promise) {
    napi_value then = GetProp(env, result, "then");
    int64_t* box = new int64_t(id);
    napi_value ok = nullptr, bad = nullptr;
    napi_create_function(env, "ok", NAPI_AUTO_LENGTH, ResolvedCb, box, &ok);
    napi_create_function(env, "bad", NAPI_AUTO_LENGTH, RejectedCb, box, &bad);
    napi_value a[2] = {ok, bad};
    CallFn(then, result, 2, a);
    return;
  }
  SendResp(env, id, result);
}

// ---- window event wiring ----
struct EvCtx {
  uint32_t id;
  int32_t wc;
  const char* ev;      // event name reported to Go
  bool extra_bounds;   // attach win.getBounds() to the payload
  bool file_drop;      // runtime flags injected on did-finish-load
  bool frameless;
  bool resizable;
};

static void MQuitApp(napi_env env);

static napi_value WindowEventCb(napi_env env, napi_callback_info info) {
  g_env = env;
  void* data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  EvCtx* ec = (EvCtx*)data;
  if (strcmp(ec->ev, "closed") == 0) {
    WinEvent(env, ec->id, "closed");
    auto it = g_windows.find(ec->id);
    if (it != g_windows.end()) {
      napi_delete_reference(env, it->second);
      g_windows.erase(it);
    }
    g_by_wc.erase(ec->wc);
    return nullptr;
  }
  if (ec->extra_bounds) {
    napi_value win = nullptr;
    auto it = g_windows.find(ec->id);
    if (it != g_windows.end()) win = RefV(it->second);
    if (!win) {
      WinEvent(env, ec->id, ec->ev);
      return nullptr;
    }
    napi_value bounds = MCallWin(env, win, nullptr, "getBounds");
    if (bounds) {
      WinEventExtra(env, ec->id, ec->ev, bounds);
      return nullptr;
    }
  }
  WinEvent(env, ec->id, ec->ev);
  return nullptr;
}

static napi_value ConsoleMessageCb(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 3) return nullptr;
  std::string msg;
  if (U8(env, argv[2], &msg)) {
    fprintf(stderr, "[renderer] %s\n", msg.c_str());
  }
  return nullptr;
}

static napi_value RenderGoneCb(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 2;
  napi_value argv[2];
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  EvCtx* ec = (EvCtx*)data;
  std::string reason;
  if (argc >= 2 && argv[1]) {
    napi_value r = GetProp(env, argv[1], "reason");
    U8(env, r, &reason);
  }
  napi_value extra = nullptr;
  napi_create_object(env, &extra);
  napi_set_named_property(env, extra, "reason", NF(env, reason.c_str()));
  WinEventExtra(env, ec->id, "render-gone", extra);
  return nullptr;
}

// did-finish-load: the renderer probe + native-transport injection +
// the draggable-region mirror (same script as the old main.js injector).
static napi_value InjectDoneOkCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string extra;
  if (argc >= 1) U8(env, argv[0], &extra);
  ELog("native init injected ok %s", extra.c_str());
  return nullptr;
}

static napi_value InjectDoneErrCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string e = "?";
  if (argc >= 1) U8(env, argv[0], &e);
  ELog("native init inject failed: %s", e.c_str());
  return nullptr;
}

static napi_value ProbeOkCb(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 1;
  napi_value argv[1];
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  EvCtx* ec = (EvCtx*)data;
  std::string probe = "<no result>";
  if (argc >= 1 && argv[0]) {
    napi_value sv = MJsonStringify(env, argv[0]);
    napi_valuetype st = napi_undefined;
    if (sv) napi_typeof(env, sv, &st);
    if (st == napi_string) {
      U8(env, sv, &probe);
    } else {
      U8(env, argv[0], &probe);
    }
  }
  ELog("renderer probe: %s", probe.c_str());
  if (g_cfg_native_addon.empty() || g_cfg_bridge_path.empty()) return nullptr;
  // rebuild the config JSON safely via the engine's own stringify
  napi_value cfg = nullptr;
  napi_create_object(env, &cfg);
  napi_set_named_property(env, cfg, "addon", NF(env, g_cfg_native_addon.c_str()));
  napi_set_named_property(env, cfg, "endpoint", NF(env, g_cfg_bridge_path.c_str()));
  napi_set_named_property(env, cfg, "token", NF(env, g_cfg_bridge_token.c_str()));
  napi_value cfg_json = MJsonStringify(env, cfg);
  std::string cj;
  if (!cfg_json || !U8(env, cfg_json, &cj)) return nullptr;
  std::string script =
      "window._wails=window._wails||{};window._wails.flags=window._wails.flags||{};";
  script += "window._wails.flags.enableFileDrop=";
  script += ec->file_drop ? "true" : "false";
  script += ";window._wails.flags.frameless=";
  script += ec->frameless ? "true" : "false";
  script += ";if(window._wails.setResizable)window._wails.setResizable(";
  script += ec->resizable ? "true" : "false";
  script += ");";
  script += ";typeof __wailsNativeInit === 'function' && __wailsNativeInit(";
  script += cj;
  script += ")";
  script += ";'dnd=' + typeof window.chrome.webview.postMessageWithAdditionalObjects + ';mirror=' + !!window.__wailsAppRegionMirror";
  // Draggable-region mirror: Wails pages mark drag surfaces with the
  // --wails-draggable custom property, while Electron moves frameless
  // windows natively via -webkit-app-region (the WM-conducted protocol
  // cannot grab while the renderer holds the pointer). Mirror the custom
  // property onto the hovered element so Chromium conducts the move
  // itself — in-process, no cross-process grab contention.
  script += ";(function(){"
            "if(window.__wailsAppRegionMirror)return;"
            "window.__wailsAppRegionMirror=true;"
            "var el=null;"
            "function clear(){if(el){el.style.setProperty('-webkit-app-region','');el=null;}}"
            "window.addEventListener('mousemove',function(ev){"
            "var t=ev.target;"
            "if(!(t instanceof Element)){clear();return;}"
            "var mode=getComputedStyle(t).getPropertyValue('--wails-draggable').trim();"
            "if(mode==='drag'){"
            "if(el!==t){clear();el=t;t.style.setProperty('-webkit-app-region','drag');}"
            "}else if(el){clear();}"
            "},true);"
            "window.addEventListener('mousedown',function(ev){"
            "if(el&&ev.target===el){el.style.setProperty('-webkit-app-region','drag');}"
            "},true);"
            "})()";
  // this = the webContents the event fired on (bound as cb this)
  napi_value thisv = nullptr, exec = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisv, nullptr);
  if (thisv) exec = GetProp(env, thisv, "executeJavaScript");
  if (!exec) {
    ELog("probe executeJavaScript failed: no fn");
    return nullptr;
  }
  napi_value iargv[1] = {NF(env, script.c_str())};
  napi_value prom = CallFn(exec, thisv, 1, iargv);
  if (!prom) {
    ELog("probe executeJavaScript failed: inject call");
    return nullptr;
  }
  bool is_promise = false;
  napi_is_promise(env, prom, &is_promise);
  if (is_promise) {
    int64_t* box = new int64_t(ec->id);
    napi_value then = GetProp(env, prom, "then");
    napi_value ok = nullptr, bad = nullptr;
    napi_create_function(env, "ok", NAPI_AUTO_LENGTH, InjectDoneOkCb, box, &ok);
    napi_create_function(env, "bad", NAPI_AUTO_LENGTH, InjectDoneErrCb, box, &bad);
    napi_value a[2] = {ok, bad};
    CallFn(then, prom, 2, a);
  }
  return nullptr;
}

static void WireWindow(napi_env env, uint32_t id, int32_t wc,
                       napi_value win, napi_value webcontents,
                       bool file_drop, bool frameless, bool resizable) {
  static const struct {
    const char* dom;  // Electron event name
    const char* go;   // name reported to Go
    bool bounds;
  } kEvents[] = {
      {"close", "close", false},         {"closed", "closed", false},
      {"focus", "focus", false},         {"blur", "blur", false},
      {"show", "show", false},           {"hide", "hide", false},
      {"maximize", "maximise", false},   {"unmaximize", "unmaximise", false},
      {"minimize", "minimise", false},   {"restore", "restore", false},
      {"enter-full-screen", "fullscreen", false},
      {"leave-full-screen", "unfullscreen", false},
      {"resize", "resize", true},        {"move", "move", true},
  };
  for (const auto& e : kEvents) {
    EvCtx* ec = new EvCtx{id, wc, e.go, e.bounds, false, false, false};
    napi_value fn = nullptr;
    napi_create_function(env, e.go, NAPI_AUTO_LENGTH, WindowEventCb, ec, &fn);
    napi_value on = GetProp(env, win, "on");
    napi_value argv[2] = {NF(env, e.dom), fn};
    CallFn(on, win, 2, argv);
  }
  // renderer observability
  {
    napi_value fn = nullptr;
    napi_create_function(env, "cm", NAPI_AUTO_LENGTH, ConsoleMessageCb,
                         nullptr, &fn);
    napi_value on = GetProp(env, webcontents, "on");
    napi_value argv[2] = {NF(env, "console-message"), fn};
    CallFn(on, webcontents, 2, argv);
  }
  {
    EvCtx* ec = new EvCtx{id, wc, "render-gone", false, false, false, false};
    napi_value fn = nullptr;
    napi_create_function(env, "rg", NAPI_AUTO_LENGTH, RenderGoneCb, ec, &fn);
    napi_value on = GetProp(env, webcontents, "on");
    napi_value argv[2] = {NF(env, "render-process-gone"), fn};
    CallFn(on, webcontents, 2, argv);
  }
  // did-finish-load: probe + inject
  {
    EvCtx* ec =
        new EvCtx{id, wc, "did-finish-load", false, file_drop, frameless, resizable};
    napi_value fn = nullptr;
    napi_create_function(env, "dfl", NAPI_AUTO_LENGTH, ProbeOkCb, ec, &fn);
    napi_value on = GetProp(env, webcontents, "on");
    napi_value argv[2] = {NF(env, "did-finish-load"), fn};
    CallFn(on, webcontents, 2, argv);
  }
}

static napi_value WinCreate(napi_env env, napi_value p) {
  napi_value opts = nullptr;
  napi_create_object(env, &opts);
  double x = MPNum(env, p, "x", 0), y = MPNum(env, p, "y", 0);
  napi_value xv = GetProp(env, p, "x");
  napi_value yv = GetProp(env, p, "y");
  napi_valuetype xt = napi_undefined, yt = napi_undefined;
  if (xv) napi_typeof(env, xv, &xt);
  if (yv) napi_typeof(env, yv, &yt);
  napi_value numv = nullptr;
  if (xt == napi_number) {
    napi_create_double(env, x, &numv);
    napi_set_named_property(env, opts, "x", numv);
  }
  if (yt == napi_number) {
    napi_create_double(env, y, &numv);
    napi_set_named_property(env, opts, "y", numv);
  }
  napi_create_double(env, MPNum(env, p, "width", 800), &numv);
  napi_set_named_property(env, opts, "width", numv);
  napi_create_double(env, MPNum(env, p, "height", 600), &numv);
  napi_set_named_property(env, opts, "height", numv);
  std::string title = MPStr(env, p, "title");
  napi_set_named_property(env, opts, "title", NF(env, title.c_str()));
  bool frameless = MPBool(env, p, "frameless");
  napi_value bv = nullptr;
  napi_get_boolean(env, !frameless, &bv);
  napi_set_named_property(env, opts, "frame", bv);
  bool transparent = MPBool(env, p, "transparent");
#ifdef __linux__
  // Chromium/ozone paints frameless+resizable windows with a dark
  // non-client resize border (the "black edges" users report vs the
  // WebKitGTK backend). A transparent window removes it; page content
  // is painted opaque, so this only enables the ARGB visual.
  if (frameless) transparent = true;
#endif
  napi_get_boolean(env, transparent, &bv);
  napi_set_named_property(env, opts, "transparent", bv);
  // JS: resizable: p.resizable !== false  (undefined -> true)
  bool resizable = true;
  {
    napi_value rv = GetProp(env, p, "resizable");
    napi_valuetype rt = napi_undefined;
    if (rv) napi_typeof(env, rv, &rt);
    if (rt == napi_boolean) {
      bool rb = true;
      napi_get_value_bool(env, rv, &rb);
      resizable = rb;
    }
  }
  napi_get_boolean(env, resizable, &bv);
  napi_set_named_property(env, opts, "resizable", bv);
  napi_get_boolean(env, MPBool(env, p, "alwaysOnTop"), &bv);
  napi_set_named_property(env, opts, "alwaysOnTop", bv);
  napi_get_boolean(env, true, &bv);
  napi_set_named_property(env, opts, "show", bv);
  if (transparent) {
    napi_set_named_property(env, opts, "backgroundColor", NF(env, "#00000000"));
  }
  napi_value webprefs = nullptr;
  napi_create_object(env, &webprefs);
  std::string preload = MPStr(env, p, "preload");
  if (preload.empty()) preload = g_cfg_preload;
  napi_set_named_property(env, webprefs, "preload", NF(env, preload.c_str()));
  napi_get_boolean(env, false, &bv);
  napi_set_named_property(env, webprefs, "contextIsolation", bv);
  napi_set_named_property(env, webprefs, "nodeIntegration", bv);
  napi_set_named_property(env, webprefs, "sandbox", bv);
  napi_set_named_property(env, opts, "webPreferences", webprefs);

  napi_value ctor = RefV(g_winctor);
  if (!ctor) return nullptr;
  napi_value cargv[1] = {opts};
  napi_value win = nullptr;
  if (napi_new_instance(env, ctor, 1, cargv, &win) != napi_ok || !win) {
    return nullptr;
  }

  uint32_t id = (uint32_t)MPNum(env, p, "id", 0);
  napi_ref wref = nullptr;
  napi_create_reference(env, win, 1, &wref);
  g_windows[id] = wref;
  napi_value webcontents = GetProp(env, win, "webContents");
  int32_t wc_id = (int32_t)MPNum(env, webcontents, "id", -1);
  g_by_wc[wc_id] = id;

  WireWindow(env, id, wc_id, win, webcontents, MPBool(env, p, "enableFileDrop"),
             MPBool(env, p, "frameless"), MPBool(env, p, "resizable"));

  std::string url = MPStr(env, p, "url");
  if (!url.empty()) {
    MCallWin1(env, win, nullptr, "loadURL", NF(env, url.c_str()));
  }
  return MCallWin(env, win, nullptr, "getBounds");
}

// ---- method dispatch ----
static napi_value DispatchMethod(napi_env env, const std::string& m,
                                 napi_value p) {
  auto simple = [&](const char* obj_key, const char* method) {
    return MCallWin(env, MGetWin(env, p), obj_key, method);
  };
  auto simple1 = [&](const char* obj_key, const char* method, napi_value a) {
    return MCallWin1(env, MGetWin(env, p), obj_key, method, a);
  };
  napi_value bv = nullptr, numv = nullptr;

  if (m == "create") return WinCreate(env, p);
  if (m == "close" || m == "destroy") return simple(nullptr, "destroy");
  if (m == "show") return simple(nullptr, "show");
  if (m == "hide") return simple(nullptr, "hide");
  if (m == "focus") {
    napi_value w = MGetWin(env, p);
    napi_value min = MCallWin(env, w, nullptr, "isMinimized");
    if (min) {
      bool mb = false;
      napi_get_value_bool(env, min, &mb);
      if (mb) MCallWin(env, w, nullptr, "restore");
    }
    return MCallWin(env, w, nullptr, "focus");
  }
  if (m == "setTitle")
    return simple1(nullptr, "setTitle", NF(env, MPStr(env, p, "title").c_str()));
  if (m == "setPosition") {
    napi_value w = MGetWin(env, p);
    napi_value argv[2] = {nullptr, nullptr};
    napi_create_double(env, MPNum(env, p, "x", 0), &argv[0]);
    napi_create_double(env, MPNum(env, p, "y", 0), &argv[1]);
    napi_value fn = GetProp(env, w, "setPosition");
    return CallFn(fn, w, 2, argv);
  }
  if (m == "setSize") {
    napi_value w = MGetWin(env, p);
    napi_value argv[2] = {nullptr, nullptr};
    napi_create_double(env, MPNum(env, p, "width", 0), &argv[0]);
    napi_create_double(env, MPNum(env, p, "height", 0), &argv[1]);
    napi_value fn = GetProp(env, w, "setSize");
    return CallFn(fn, w, 2, argv);
  }
  if (m == "setBounds") {
    napi_value w = MGetWin(env, p);
    napi_value b = nullptr;
    napi_create_object(env, &b);
    const char* keys[4] = {"x", "y", "width", "height"};
    for (const char* k : keys) {
      napi_value v = GetProp(env, p, k);
      napi_valuetype vt = napi_undefined;
      if (v) napi_typeof(env, v, &vt);
      if (vt == napi_number) napi_set_named_property(env, b, k, v);
    }
    napi_value fn = GetProp(env, w, "setBounds");
    return CallFn(fn, w, 1, &b);
  }
  if (m == "getBounds") return simple(nullptr, "getBounds");
  if (m == "center") return simple(nullptr, "center");
  if (m == "setAlwaysOnTop") {
    napi_get_boolean(env, MPBool(env, p, "v"), &bv);
    return simple1(nullptr, "setAlwaysOnTop", bv);
  }
  if (m == "setResizable") {
    napi_get_boolean(env, MPBool(env, p, "v"), &bv);
    return simple1(nullptr, "setResizable", bv);
  }
  if (m == "setFullScreen") {
    napi_get_boolean(env, MPBool(env, p, "v"), &bv);
    return simple1(nullptr, "setFullScreen", bv);
  }
  if (m == "maximise") return simple(nullptr, "maximize");
  if (m == "unmaximise") return simple(nullptr, "unmaximize");
  if (m == "minimise") return simple(nullptr, "minimize");
  if (m == "unminimise") return simple(nullptr, "restore");
  if (m == "isVisible") return simple(nullptr, "isVisible");
  if (m == "isFocused") return simple(nullptr, "isFocused");
  if (m == "isMinimised") return simple(nullptr, "isMinimized");
  if (m == "isMaximised") return simple(nullptr, "isMaximized");
  if (m == "isFullScreen") return simple(nullptr, "isFullScreen");
  if (m == "execJS")
    return simple1("webContents", "executeJavaScript",
                   NF(env, MPStr(env, p, "js").c_str()));
  if (m == "loadURL")
    return simple1(nullptr, "loadURL", NF(env, MPStr(env, p, "url").c_str()));
  if (m == "reload") return simple("webContents", "reload");
  if (m == "forceReload") return simple("webContents", "reloadIgnoringCache");
  if (m == "openDevTools") {
    napi_value w = MGetWin(env, p);
    napi_value wc = GetProp(env, w, "webContents");
    napi_value mode = nullptr;
    napi_create_object(env, &mode);
    napi_set_named_property(env, mode, "mode", NF(env, "detach"));
    napi_value fn = GetProp(env, wc, "openDevTools");
    return CallFn(fn, wc, 1, &mode);
  }
  if (m == "setZoom") {
    napi_create_double(env, MPNum(env, p, "v", 1), &numv);
    return simple1("webContents", "setZoomFactor", numv);
  }
  if (m == "getZoom") return simple("webContents", "getZoomFactor");
  if (m == "setBackgroundColour")
    return simple1(nullptr, "setBackgroundColor",
                   NF(env, MPStr(env, p, "colour").c_str()));
  if (m == "setIgnoreMouseEvents") {
    napi_get_boolean(env, MPBool(env, p, "v"), &bv);
    return simple1(nullptr, "setIgnoreMouseEvents", bv);
  }
  if (m == "copy") return simple("webContents", "copy");
  if (m == "paste") return simple("webContents", "paste");
  if (m == "cut") return simple("webContents", "cut");
  if (m == "undo") return simple("webContents", "undo");
  if (m == "redo") return simple("webContents", "redo");
  if (m == "selectAll") return simple("webContents", "selectAll");
  if (m == "delete") return simple("webContents", "delete");
  if (m == "setMinimumSize" || m == "setMaximumSize") {
    napi_value w = MGetWin(env, p);
    napi_value argv[2] = {nullptr, nullptr};
    napi_create_double(env, MPNum(env, p, "width", 0), &argv[0]);
    napi_create_double(env, MPNum(env, p, "height", 0), &argv[1]);
    napi_value fn = GetProp(env, w, m.c_str());
    return CallFn(fn, w, 2, argv);
  }
  if (m == "setParent") {
    napi_value w = MGetWin(env, p);
    uint32_t parent_id = (uint32_t)MPNum(env, p, "parent", 0);
    napi_value parent = nullptr;
    auto it = g_windows.find(parent_id);
    if (it != g_windows.end()) parent = RefV(it->second);
    if (!parent) {
      napi_throw_error(env, nullptr, "setParent: unknown parent window");
      return nullptr;
    }
    napi_value fn = GetProp(env, w, "setParentWindow");
    return CallFn(fn, w, 1, &parent);
  }
  if (m == "startDrag") {
    // Frameless MOVES are conducted natively by Chromium (the
    // draggable-region mirror lets it ungrab in-process) — verified
    // working on the real desktop; a no-op here.
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }
  if (m == "startResize") {
#ifndef __linux__
    // Windows/macOS: Chromium's native frameless edge resize handles
    // the gesture (invisible hit borders, no painted frame).
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
#else
    // Frameless RESIZE on Linux: Chromium's own edge handling is
    // unreliable across WMs and the transparent window's input region
    // goes stale — conduct the resize here via the X11 pointer loop.
    napi_value w = MGetWin(env, p);
    napi_value gnh = GetProp(env, w, "getNativeWindowHandle");
    napi_value handle = CallFn(gnh, w, 0, nullptr);
    bool is_view = false;
    if (!handle || napi_is_typedarray(env, handle, &is_view) != napi_ok ||
        !is_view) {
      napi_throw_error(env, nullptr, "startResize: no native window handle");
      return nullptr;
    }
    napi_typedarray_type tt = (napi_typedarray_type)0;
    size_t blen = 0, boff = 0;
    void* bdata = nullptr;
    napi_value ab = nullptr;
    if (napi_get_typedarray_info(env, handle, &tt, &blen, &bdata, &ab,
                                 &boff) != napi_ok ||
        blen < 4 || !bdata) {
      napi_throw_error(env, nullptr, "startResize: short native handle");
      return nullptr;
    }
    unsigned long xid = *reinterpret_cast<const uint32_t*>(
        static_cast<const char*>(bdata) + boff);
    int direction = x11_direction(MPStr(env, p, "edge").c_str());
    x11_move_resize(xid, direction);
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
#endif
  }
  if (m == "setEnabled") {
    napi_get_boolean(env, MPBool(env, p, "v"), &bv);
    return simple1(nullptr, "setEnabled", bv);
  }
  if (m == "print") return simple("webContents", "print");
  if (m == "getScreens") {
    napi_value nv = nullptr;
    // electron.screen is only valid once the app is ready — every
    // dispatch happens post-ready
    napi_value electron = RefV(g_electron);
    napi_value screen = electron ? GetProp(env, electron, "screen") : nullptr;
    napi_value displays = screen ? CallFn(GetProp(env, screen, "getAllDisplays"), screen, 0, nullptr) : nullptr;
    napi_value primary = screen ? CallFn(GetProp(env, screen, "getPrimaryDisplay"), screen, 0, nullptr) : nullptr;
    if (!displays || !primary) {
      napi_throw_error(env, nullptr, "getScreens: screen module unavailable");
      return nullptr;
    }
    napi_value primary_id = GetProp(env, primary, "id");
    uint32_t count = 0;
    napi_get_array_length(env, displays, &count);
    napi_value out = nullptr;
    napi_create_array_with_length(env, count, &out);
    for (uint32_t i = 0; i < count; i++) {
      napi_value d = nullptr, o = nullptr;
      napi_get_element(env, displays, i, &d);
      napi_create_object(env, &o);
      // ID: stringified display id (stable within the session)
      napi_value idv = GetProp(env, d, "id");
      std::string ids;
      if (idv) U8(env, idv, &ids);
      if (ids.empty() && idv) {
        double dd = 0;
        napi_get_value_double(env, idv, &dd);
        ids = std::to_string((long long)dd);
      }
      napi_set_named_property(env, o, "ID", NF(env, ids.c_str()));
      napi_value label = GetProp(env, d, "label");
      std::string label_s;
      if (label) U8(env, label, &label_s);
      napi_set_named_property(env, o, "Name", NF(env, label_s.c_str()));
      auto num = [&](napi_value obj, const char* k) -> double {
        if (!obj) return 0;
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, obj, &vt);
        if (vt != napi_object) return 0;  // property reads on primitives throw
        napi_value v = GetProp(env, obj, k);
        double d2 = 0;
        if (v) napi_get_value_double(env, v, &d2);
        return d2;
      };
      auto rect = [&](napi_value o2, const char* k, const char* src_key,
                      napi_value src, double scale) {
        napi_value r = GetProp(env, src, src_key);
        if (!r) return;
        napi_valuetype rt = napi_undefined;
        napi_typeof(env, r, &rt);
        if (rt != napi_object) return;
        napi_value ro = nullptr;
        napi_create_object(env, &ro);
        napi_value nv = nullptr;
        napi_create_double(env, num(r, "x") * scale, &nv);
        napi_set_named_property(env, ro, "X", nv);
        napi_create_double(env, num(r, "y") * scale, &nv);
        napi_set_named_property(env, ro, "Y", nv);
        napi_create_double(env, num(r, "width") * scale, &nv);
        napi_set_named_property(env, ro, "Width", nv);
        napi_create_double(env, num(r, "height") * scale, &nv);
        napi_set_named_property(env, ro, "Height", nv);
        napi_set_named_property(env, o2, k, ro);
      };
      double sf = num(d, "scaleFactor");
      if (sf <= 0) sf = 1;
      napi_value sfv = nullptr;
      napi_create_double(env, sf, &sfv);
      napi_set_named_property(env, o, "ScaleFactor", sfv);
      napi_value bounds0 = GetProp(env, d, "bounds");
      napi_create_double(env, num(bounds0, "x"), &nv);
      napi_set_named_property(env, o, "X", nv);
      napi_create_double(env, num(bounds0, "y"), &nv);
      napi_set_named_property(env, o, "Y", nv);
      rect(o, "Bounds", "bounds", d, 1.0);
      napi_value bounds = GetProp(env, d, "bounds");
      napi_value sizeo = nullptr;
      napi_create_object(env, &sizeo);
      napi_create_double(env, num(bounds, "width"), &nv);
      napi_set_named_property(env, sizeo, "Width", nv);
      napi_create_double(env, num(bounds, "height"), &nv);
      napi_set_named_property(env, sizeo, "Height", nv);
      napi_set_named_property(env, o, "Size", sizeo);
      rect(o, "PhysicalBounds", "bounds", d, sf);
      rect(o, "WorkArea", "workArea", d, 1.0);
      rect(o, "PhysicalWorkArea", "workArea", d, sf);
      bool is_primary = false;
      napi_value pid = primary_id;
      double a2 = 0, b2 = 0;
      if (pid && idv) {
        napi_get_value_double(env, pid, &a2);
        napi_get_value_double(env, idv, &b2);
        is_primary = a2 == b2;
      }
      napi_get_boolean(env, is_primary, &bv);
      napi_set_named_property(env, o, "IsPrimary", bv);
      double rot = num(d, "rotation");
      napi_create_double(env, rot, &nv);
      napi_set_named_property(env, o, "Rotation", nv);
      napi_set_element(env, out, i, o);
    }
    return out;
  }
  if (m == "showContextMenu") {
    // Forward the whole spec to main.js's __wailsShowContextMenu: the
    // menu build + popup stay in pure JS (see the note in main.js — the
    // napi-built path never renders the popup on linux). The click
    // closures write contextmenu-select events straight to stdout.
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value fn = GetProp(env, global, "__wailsShowContextMenu");
    if (!fn) {
      napi_throw_error(env, nullptr,
                       "showContextMenu: __wailsShowContextMenu missing");
      return nullptr;
    }
    napi_value jv = MJsonStringify(env, p);
    if (!jv) return nullptr;
    napi_value argv[1] = {jv};
    return CallFn(fn, global, 1, argv);
  }
  if (m == "showOpenDialog" || m == "showSaveDialog" || m == "showMessageDialog") {
    // Go sends Electron-shaped options; copy the known fields onto the
    // options object and hand back the dialog promise (RespondValue is
    // promise-aware, so the reply flows back when it resolves).
    napi_value electron = RefV(g_electron);
    napi_value dialog = electron ? GetProp(env, electron, "dialog") : nullptr;
    if (!dialog) {
      napi_throw_error(env, nullptr, "dialog module unavailable");
      return nullptr;
    }
    napi_value opts = nullptr;
    napi_create_object(env, &opts);
    for (const char* k : {"title", "filters", "properties", "defaultPath",
                          "type", "message", "buttons", "defaultId",
                          "cancelId"}) {
      napi_value v = GetProp(env, p, k);
      napi_valuetype vt = napi_undefined;
      if (v) napi_typeof(env, v, &vt);
      if (vt != napi_undefined) napi_set_named_property(env, opts, k, v);
    }
    if (napi_value b = GetProp(env, p, "button")) {
      napi_valuetype bt = napi_undefined;
      napi_typeof(env, b, &bt);
      if (bt == napi_string) napi_set_named_property(env, opts, "buttonLabel", b);
    }
    const char* fn_name = m == "showMessageDialog" ? "showMessageBox"
                          : m == "showSaveDialog"  ? "showSaveDialog"
                                                   : "showOpenDialog";
    napi_value fn = GetProp(env, dialog, fn_name);
    if (!fn) {
      napi_throw_error(env, nullptr, "dialog fn unavailable");
      return nullptr;
    }
    uint32_t wid = (uint32_t)MPNum(env, p, "windowID", 0);
    napi_value prom = nullptr;
    if (wid != 0) {
      napi_value win = nullptr;
      auto it = g_windows.find(wid);
      if (it != g_windows.end()) win = RefV(it->second);
      if (win) {
        napi_value a[2] = {win, opts};
        prom = CallFn(fn, dialog, 2, a);
      }
    }
    if (!prom) prom = CallFn(fn, dialog, 1, &opts);
    return prom;
  }
  if (m == "quit") {
    napi_value app = RefV(g_app);
    napi_value fn = app ? GetProp(env, app, "quit") : nullptr;
    return CallFn(fn, app, 0, nullptr);
  }
  napi_throw_error(env, nullptr, ("unknown method " + m).c_str());
  return nullptr;
}

// ---- stdin control reader (raw blocking thread — no libuv stream, so
// window destruction can never stall the poll; EOF = host gone) ----
static void MQuitApp(napi_env env) {
  napi_value app = RefV(g_app);
  napi_value fn = app ? GetProp(env, app, "quit") : nullptr;
  CallFn(fn, app, 0, nullptr);
}

static void LinesAsyncCb(uv_async_t*) {
  napi_env env = g_env;
  // a raw uv callback has no implicit napi HandleScope — every napi call
  // below would run scopeless (undefined behavior: garbage property
  // reads) unless we open one ourselves.
  napi_handle_scope hs = nullptr;
  if (napi_open_handle_scope(env, &hs) != napi_ok) return;
  std::vector<std::string> batch;
  {
    std::lock_guard<std::mutex> lk(g_lines_mu);
    batch.swap(g_lines);
  }
  for (auto& line : batch) {
    if (line == "\x01" "EOF") {
      if (g_m_debug) ELog("control read ended (eof)");
      MQuitApp(env);
      return;
    }
    if (line.empty()) continue;
    napi_value parsed = MJsonParse(env, NF(env, line.c_str()));
    if (!parsed) {
      napi_value exc = nullptr;
      napi_get_and_clear_last_exception(env, &exc);
      continue;
    }
    napi_valuetype pt = napi_undefined;
    napi_typeof(env, parsed, &pt);
    if (pt != napi_object) continue;
    napi_value tv = GetProp(env, parsed, "t");
    std::string t;
    if (U8(env, tv, &t) && t == "resp") continue;  // no Electron->Go reqs
    napi_value mv = GetProp(env, parsed, "m");
    std::string method;
    if (!U8(env, mv, &method) || method.empty()) continue;
    int64_t id = (int64_t)MPNum(env, parsed, "id", -1);
    napi_value params = nullptr;
    napi_create_object(env, &params);
    napi_value pv = GetProp(env, parsed, "p");
    napi_valuetype pvt = napi_undefined;
    if (pv) napi_typeof(env, pv, &pvt);
    if (pvt == napi_object) params = pv;
    if (g_m_debug) ELog("dispatch %s id=%lld", method.c_str(), (long long)id);
    napi_value result = DispatchMethod(env, method, params);
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value exc = nullptr;
      napi_get_and_clear_last_exception(env, &exc);
      std::string err = "Error";
      if (exc) {
        napi_value s = nullptr;
        if (napi_coerce_to_string(env, exc, &s) == napi_ok && s) {
          U8(env, s, &err);
        }
      }
      SendRespErr(env, id, err.c_str());
      continue;
    }
    RespondValue(env, id, result);
  }
  napi_close_handle_scope(env, hs);
}

static void QuitAsyncCb(uv_async_t*) {
  napi_env env = g_env;
  napi_handle_scope hs = nullptr;
  if (napi_open_handle_scope(env, &hs) != napi_ok) return;
  MQuitApp(env);
  napi_close_handle_scope(env, hs);
}

static void* ReaderThread(void*) {
  std::string buf;
  char tmp[65536];
  for (;;) {
    ssize_t n = read(0, tmp, sizeof(tmp));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    buf.append(tmp, static_cast<size_t>(n));
    size_t idx;
    while ((idx = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, idx);
      buf.erase(0, idx + 1);
      {
        std::lock_guard<std::mutex> lk(g_lines_mu);
        g_lines.push_back(std::move(line));
      }
      uv_async_send(&g_lines_async);
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_lines_mu);
    g_lines.push_back("\x01" "EOF");
  }
  uv_async_send(&g_lines_async);
  return nullptr;
}

#ifndef _WIN32
// orphan protection layer 2 (POSIX only): quit when re-parented (host
// SIGKILL'd). A dead host on Windows is reaped by electron.go's job
// teardown instead.
static void* PpidGuardThread(void*) {
  if (g_m_debug) ELog("guard armed, originalPpid=%u", g_orig_ppid);
  for (;;) {
    struct timespec ts = {2, 0};
    nanosleep(&ts, nullptr);
    uint32_t ppid = static_cast<uint32_t>(getppid());
    if (g_m_debug) {
      fprintf(stderr, "[wails-electron] beat, ppid=%u\n", ppid);
    }
    if (ppid != g_orig_ppid) {
      ELog("parent died, quitting");
      uv_async_send(&g_quit_async);
      break;
    }
  }
  return nullptr;
}
#endif  // _WIN32

static napi_value PreloadErrorCb(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string path, err;
  if (argc >= 2) U8(env, argv[1], &path);
  if (argc >= 3) U8(env, argv[2], &err);
  fprintf(stderr, "[wails-electron] preload-error %s: %s\n", path.c_str(),
          err.c_str());
  return nullptr;
}

static napi_value NoopCb(napi_env env, napi_callback_info info) {  return nullptr;
}

static napi_value WailsMessageCb(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  // event.sender.id -> windowID (numeric keys: the churning fix)
  uint32_t id = 0;
  if (argc >= 1 && argv[0]) {
    napi_value sender = GetProp(env, argv[0], "sender");
    int32_t wc = (int32_t)MPNum(env, sender, "id", -1);
    auto it = g_by_wc.find(wc);
    if (it != g_by_wc.end()) id = it->second;
  }
  napi_value payload = argc >= 2 ? argv[0 + 1] : nullptr;
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  if (!payload) payload = undef;
  // compat diagnostics passthrough
  napi_valuetype pvt = napi_undefined;
  napi_typeof(env, payload, &pvt);
  if (pvt == napi_string) {
    napi_value parsed = MJsonParse(env, payload);
    if (!parsed) {
      // non-JSON postMessages are routine (e.g. wails:runtime:ready) —
      // swallow the SyntaxError or it poisons every later napi call
      napi_value exc = nullptr;
      napi_get_and_clear_last_exception(env, &exc);
    }
    if (parsed) {
      napi_value name = GetProp(env, parsed, "name");
      std::string ns;
      if (name && U8(env, name, &ns) && ns.compare(0, 7, "compat:") == 0) {
        napi_value data = GetProp(env, parsed, "data");
        napi_value ds = nullptr;
        std::string dstr;
        if (data && napi_coerce_to_string(env, data, &ds) == napi_ok) {
          U8(env, ds, &dstr);
        }
        fprintf(stderr, "[wails-electron] %s %s\n", ns.c_str(), dstr.c_str());
      }
    }
  }
  napi_value o = nullptr, p = nullptr, idv = nullptr;
  napi_create_object(env, &o);
  napi_create_object(env, &p);
  napi_set_named_property(env, o, "t", NF(env, "ev"));
  napi_set_named_property(env, o, "e", NF(env, "message"));
  napi_create_uint32(env, id, &idv);
  napi_set_named_property(env, p, "id", idv);
  napi_set_named_property(env, p, "payload", payload);
  napi_set_named_property(env, o, "p", p);
  SendObj(env, o);
  return nullptr;
}

static napi_value CompatPingCb(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 2) {
    return argv[1];
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

static napi_value ReadyCb(napi_env env, napi_callback_info info) {
  g_env = env;
  static pthread_t reader, guard;
  pthread_create(&reader, nullptr, ReaderThread, nullptr);
#ifndef _WIN32
  pthread_create(&guard, nullptr, PpidGuardThread, nullptr);
#endif
  WinEvent(env, 0, "ready");
  return nullptr;
}

static napi_value QuitAsyncFromSignal(napi_env env, napi_callback_info info) {
  uv_async_send(&g_quit_async);
  return nullptr;
}

// mainEntry(electron, cfg): the entire former main.js bootstrap.
static napi_value MainEntry(napi_env env, napi_callback_info info) {
  g_env = env;
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    ThrowE(env, "mainEntry(electron, cfg) requires the module");
    return nullptr;
  }
  napi_value electron = argv[0];
  g_app = nullptr;
  g_winctor = nullptr;
  g_ipcmain = nullptr;
  if (electron) {
    napi_value a = GetProp(env, electron, "app");
    napi_value b = GetProp(env, electron, "BrowserWindow");
    napi_value c = GetProp(env, electron, "ipcMain");
    if (a) napi_create_reference(env, a, 1, &g_app);
    if (b) napi_create_reference(env, b, 1, &g_winctor);
    if (c) napi_create_reference(env, c, 1, &g_ipcmain);
    napi_create_reference(env, electron, 1, &g_electron);
  }
  g_m_debug = getenv("WAILS_ELECTRON_DEBUG") != nullptr &&
              strcmp(getenv("WAILS_ELECTRON_DEBUG"), "1") == 0;
#ifndef _WIN32
  g_orig_ppid = static_cast<uint32_t>(getppid());
#endif

  if (argc >= 2 && argv[1]) {
    napi_value cfg = argv[1];
    g_cfg_preload = MPStr(env, cfg, "preload");
    g_cfg_bridge_path = MPStr(env, cfg, "bridgePath");
    g_cfg_bridge_token = MPStr(env, cfg, "bridgeToken");
    g_cfg_native_addon = MPStr(env, cfg, "nativeAddon");
#ifdef _WIN32
#define br_setenv(name, value) _putenv_s(name, value)
#else
#define br_setenv(name, value) setenv(name, value, 1)
#endif
    if (!g_cfg_bridge_path.empty())
      br_setenv("WAILS_ELECTRON_BRIDGE_PATH", g_cfg_bridge_path.c_str());
    if (!g_cfg_native_addon.empty())
      br_setenv("WAILS_ELECTRON_NATIVE_ADDON", g_cfg_native_addon.c_str());
  }

  // Chromium switches: must land before app ready.
  {
    napi_value app = RefV(g_app);
    napi_value cl = app ? GetProp(env, app, "commandLine") : nullptr;
    napi_value append = cl ? GetProp(env, cl, "appendSwitch") : nullptr;
    if (append) {
      napi_value a[2] = {NF(env, "disable-features"),
                         NF(env, "CalculateNativeWinOcclusion")};
      CallFn(append, cl, 2, a);
      const char* sw = getenv("WAILS_ELECTRON_SWITCHES");
      if (sw != nullptr) {
        std::string s(sw);
        size_t pos = 0;
        while (pos < s.size()) {
          size_t e = s.find(' ', pos);
          if (e == std::string::npos) e = s.size();
          std::string tok = s.substr(pos, e - pos);
          pos = e + 1;
          if (!tok.empty()) {
            napi_value a1[1] = {NF(env, tok.c_str())};
            CallFn(append, cl, 1, a1);
          }
        }
      }
    }
  }
  const char* nogpu = getenv("WAILS_ELECTRON_DISABLE_GPU");
  if (nogpu != nullptr && strcmp(nogpu, "1") == 0) {
    napi_value app = RefV(g_app);
    napi_value fn = app ? GetProp(env, app, "disableHardwareAcceleration") : nullptr;
    CallFn(fn, app, 0, nullptr);
  }

  // app event wiring
  {
    napi_value app = RefV(g_app);
    napi_value on = app ? GetProp(env, app, "on") : nullptr;
    const char* evs[2] = {"preload-error", "window-all-closed"};
    napi_callback_info_unused_marker:;
    for (const char* ev : evs) {
      napi_callback cb = strcmp(ev, "preload-error") == 0 ? PreloadErrorCb : NoopCb;
      napi_value fn = nullptr;
      napi_create_function(env, ev, NAPI_AUTO_LENGTH, cb, nullptr, &fn);
      napi_value a[2] = {NF(env, ev), fn};
      CallFn(on, app, 2, a);
    }
  }

  // ipcMain handlers
  {
    napi_value ipc = RefV(g_ipcmain);
    napi_value on = ipc ? GetProp(env, ipc, "on") : nullptr;
    napi_value fn = nullptr;
    napi_create_function(env, "wm", NAPI_AUTO_LENGTH, WailsMessageCb, nullptr, &fn);
    napi_value a[2] = {NF(env, "wails:message"), fn};
    CallFn(on, ipc, 2, a);
    napi_value handle = ipc ? GetProp(env, ipc, "handle") : nullptr;
    napi_value hf = nullptr;
    napi_create_function(env, "ping", NAPI_AUTO_LENGTH, CompatPingCb, nullptr, &hf);
    napi_value h[2] = {NF(env, "compat:ping"), hf};
    CallFn(handle, ipc, 2, h);
  }

  // signals
  {
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value proc = GetProp(env, global, "process");
    napi_value onfn = proc ? GetProp(env, proc, "on") : nullptr;
    for (const char* sig : {"SIGTERM", "SIGINT"}) {
      napi_value fn = nullptr;
      napi_create_function(env, sig, NAPI_AUTO_LENGTH, QuitAsyncFromSignal, nullptr, &fn);
      napi_value a[2] = {NF(env, sig), fn};
      CallFn(onfn, proc, 2, a);
    }
  }

  // async pumps: line dispatch + quit, on this loop
  if (napi_get_uv_event_loop(env, &g_loop) != napi_ok || g_loop == nullptr) {
    ThrowE(env, "mainEntry: no uv loop");
    return nullptr;
  }
  uv_async_init(g_loop, &g_lines_async, LinesAsyncCb);
  uv_async_init(g_loop, &g_quit_async, QuitAsyncCb);

  // app.whenReady().then(ReadyCb)
  {
    napi_value app = RefV(g_app);
    napi_value whenReady = app ? GetProp(env, app, "whenReady") : nullptr;
    napi_value prom = CallFn(whenReady, app, 0, nullptr);
    bool is_promise = false;
    if (prom && napi_is_promise(env, prom, &is_promise) == napi_ok &&
        is_promise) {
      napi_value then = GetProp(env, prom, "then");
      napi_value ok = nullptr;
      napi_create_function(env, "ready", NAPI_AUTO_LENGTH, ReadyCb, nullptr, &ok);
      CallFn(then, prom, 1, &ok);
    }
  }

  return nullptr;
}

NAPI_MODULE_INIT(/* env, exports, module, context */) {
  g_env = env;
  napi_value fn = nullptr;
  napi_create_function(env, "preloadInit", NAPI_AUTO_LENGTH, PreloadInit,
                       nullptr, &fn);
  napi_set_named_property(env, exports, "preloadInit", fn);
  napi_create_function(env, "mainEntry", NAPI_AUTO_LENGTH, MainEntry, nullptr,
                       &fn);
  napi_set_named_property(env, exports, "mainEntry", fn);
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, Close, nullptr, &fn);
  napi_set_named_property(env, exports, "close", fn);
#ifdef WAILS_BRIDGE_SERDE_TEST
  // corpus-test hooks: expose the fast paths directly so a plain-node
  // harness can byte-compare against node's v8.serialize.
  napi_create_function(
      env, "_fastSerialize", NAPI_AUTO_LENGTH,
      [](napi_env e, napi_callback_info i) -> napi_value {
        g_env = e;
        size_t c = 1;
        napi_value a[1];
        napi_get_cb_info(e, i, &c, a, nullptr, nullptr);
        SerBuf s;
        if (c < 1 || !fast_serialize(a[0], &s)) {
          napi_value u;
          napi_get_undefined(e, &u);
          return u;
        }
        void* d = nullptr;
        napi_value buf = nullptr;
        if (napi_create_buffer(e, s.b.size(), &d, &buf) != napi_ok) {
          return nullptr;
        }
        memcpy(d, s.b.data(), s.b.size());
        return buf;
      },
      nullptr, &fn);
  napi_set_named_property(env, exports, "_fastSerialize", fn);
  napi_create_function(
      env, "_fastDeserialize", NAPI_AUTO_LENGTH,
      [](napi_env e, napi_callback_info i) -> napi_value {
        g_env = e;
        size_t c = 1;
        napi_value a[1];
        napi_get_cb_info(e, i, &c, a, nullptr, nullptr);
        void* d = nullptr;
        size_t len = 0;
        if (c < 1 ||
            napi_get_buffer_info(e, a[0], &d, &len) != napi_ok) {
          return nullptr;
        }
        napi_value out = nullptr;
        if (!fast_deserialize(e, (const uint8_t*)d, len, &out)) {
          napi_value u;
          napi_get_undefined(e, &u);
          return u;
        }
        return out;
      },
      nullptr, &fn);
  napi_set_named_property(env, exports, "_fastDeserialize", fn);
#endif
  return exports;
}
