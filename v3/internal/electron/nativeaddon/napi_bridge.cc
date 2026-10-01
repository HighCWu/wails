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

// forward declarations (callback wiring order)
static napi_value FetchOverride(napi_env env, napi_callback_info info);
static napi_value NativeInitThunk(napi_env env, napi_callback_info info);
static napi_value PostMessageAdditionalCb(napi_env env,
                                          napi_callback_info info);
static napi_value NativeEcho(napi_env env, napi_callback_info info);
static napi_value NativeInvoke(napi_env env, napi_callback_info info);

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
// serde via the injected node v8 module

// Serialize any JS value to wire bytes through the injected v8.serialize
// (native ValueSerializer — the same bytes the Go decoder already speaks).
// Returns the raw pointer into the returned Buffer; the buffer stays
// alive via *buf_out on the caller's stack for the synchronous write.
static bool serialize_value(napi_value value, const uint8_t** out,
                            size_t* out_len, napi_value* buf_out) {
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

// Deserialize wire bytes through the injected v8.deserialize. Zero copy
// when an external ArrayBuffer is permitted (the finalizer owns and frees
// the heap vector); otherwise one copy into a fresh Buffer — the same
// copy v8-direct makes in its deserialize delegate. Takes ownership of
// *resp either way. Returns nullptr with an exception pending on failure.
static napi_value deserialize_bytes(std::vector<uint8_t>* resp) {
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
  return exports;
}
