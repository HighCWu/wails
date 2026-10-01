// N-API renderer addon — the Windows path of the electron backend's native
// transport. The v8-direct addon (bridge.cc) cannot exist on Windows:
// electron.exe exports only the N-API surface (napi_*/uv_*, verified on
// 44.4.5: 3242 exports, zero v8:: symbols) and no node.lib is published,
// so any v8 C++ reference fails at load time. This addon speaks the same
// protocol v3 frames against the same bridge endpoint, using only N-API —
// which electron.exe does export — and node's own v8.serialize/
// v8.deserialize injected from the preload for the wire serde (byte-for-
// byte the format the Go decoder already speaks; node wraps typed arrays
// as 0x5C host objects exactly like the Go encoder, verified empirically).
//
// Clone discipline (the whole point of choosing this over a naive N-API
// port): values cross the boundary as opaque handles; the invoke body is
// never extracted into C strings — the assembled call object is handed to
// v8.serialize and the returned Buffer's raw pointer is written to the fd
// directly (zero intermediate copy, same count as v8-direct's serializer
// buffer); responses are wrapped as an external ArrayBuffer over the
// received bytes when the runtime allows it (zero copy, strictly better
// than v8-direct's Buffer::Copy in the deserialize delegate), with a
// copying fallback if external buffer creation is refused.
//
// The main-process surface is NOT here: the control protocol is low-
// frequency JSON lines where N-API buys nothing, so windows v1 runs the
// JS control plane (assets/main-js.js) + this renderer addon.
//
// Exports: preloadInit(electron, v8), close() — same preload contract as
// bridge.cc's preloadInit plus the v8 module for serde injection (v8-
// direct ignores the second argument, so one preload.js serves both).

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
#endif
#include <pthread.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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
  X(napi_typeof, (napi_env env, napi_value value, napi_valuetype* result), (env, value, result))

extern "C" {
#define X(name, params, args)            \
  static napi_status (*name##_p) params; \
  napi_status name params { return name##_p args; }
NAPI_DYN_LIST(X)
#undef X
}

__attribute__((constructor)) static void napi_dyn_init() {
  HMODULE h = GetModuleHandleW(NULL);  // the host: electron.exe
#define X(name, params, args)                                  \
  name##_p = reinterpret_cast<napi_status(*) params>(           \
      GetProcAddress(h, #name));
  NAPI_DYN_LIST(X)
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

NAPI_MODULE_INIT(/* env, exports, module, context */) {
  g_env = env;
  napi_value fn = nullptr;
  napi_create_function(env, "preloadInit", NAPI_AUTO_LENGTH, PreloadInit,
                       nullptr, &fn);
  napi_set_named_property(env, exports, "preloadInit", fn);
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
