// go-bridge (C++/V8): the renderer-side electron interaction surface as a
// native addon. preload.js shrinks to a one-line delegation; every hot
// byte (framing, serialization, response handling) runs in C++:
//
//   preloadInit(electron)         — postMessage shim + __wailsNativeInit hook
//   __wailsNativeInit({endpoint,token,addon})
//                                 — UDS connect (frame v3) + fetch override
//   __wailsV8Serialize(value)     — ValueSerializer (node-compatible bytes)
//   __wailsV8Deserialize(bytes)   — ValueDeserializer
//   __nativeInvoke(msgObj)        — one v3 frame, response object back
//   __nativeEcho(payload)         — {channel:"echo"} round trip, sync
//   close()                       — teardown
//
// Wire format (pkg/application/webview_electron_native_linux.go): every
// frame is 4-byte LE length + ONE v8-serialized message, the same bytes
// Electron's renderer hands its main process on an IPC invoke. The
// serializer output is byte-identical to node's v8.serialize (verified by
// probe): ArrayBufferViews ride the host-object form (0x5C type len bytes)
// via SetTreatArrayBufferViewsAsHostObjects, which is what the Go decoder
// expects.
//
// Built against Electron's own node headers (NODE_MODULE + V8 C++ API,
// -fno-rtti matching node's build) — this addon is deliberately pinned to
// an Electron version range; rebuild via build-cpp.sh on bumps.
//
// Concurrency: all I/O runs synchronously on the JS thread behind one
// mutex — sequential RPC by design, mirroring the handshake.
#include <node.h>
#include <node_buffer.h>
#include <v8.h>

#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define BRIDGE_TIMEOUT_SEC 10

using namespace v8;

static int g_fd = -1;
static int g_connected = 0;
static pthread_mutex_t g_io_mu = PTHREAD_MUTEX_INITIALIZER;

// persistent JS references (single renderer isolate)
static Global<Object> g_ipc_renderer;
static Global<Function> g_orig_fetch;
static Global<Function> g_response_ctor;

static Isolate* I(const FunctionCallbackInfo<Value>& args) {
  return args.GetIsolate();
}

static Local<String> S(Isolate* isolate, const char* s) {
  return String::NewFromUtf8(isolate, s, NewStringType::kNormal)
      .ToLocalChecked();
}

static void Warn(const char* msg) {
  fprintf(stderr, "[go-bridge] %s\n", msg);
}

// ----------------------------------------------------------------------
// wire I/O (same frames as the C addon)
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
    shutdown(g_fd, SHUT_RDWR);
    close(g_fd);
    g_fd = -1;
  }
  g_connected = 0;
}

// ----------------------------------------------------------------------
// V8 value (de)serialization with node-compatible bytes
class SerDelegate : public ValueSerializer::Delegate {
 public:
  void ThrowDataCloneError(Local<String> message) override {
    // unreachable for our value shapes
  }
  bool HasCustomHostObject(Isolate*) override { return true; }
  Maybe<bool> IsHostObject(Isolate*, Local<Object> object) override {
    if (object->IsArrayBufferView()) return Just(true);
    return Just(false);
  }
  Maybe<bool> WriteHostObject(Isolate*, Local<Object> object) override {
    Local<ArrayBufferView> view = object.As<ArrayBufferView>();
    std::shared_ptr<BackingStore> bs = view->Buffer()->GetBackingStore();
    size_t off = view->ByteOffset(), len = view->ByteLength();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(bs->Data()) + off;
    ser_->WriteUint32(1);  // node host-object type id 1 = typed array
    ser_->WriteUint32(static_cast<uint32_t>(len));
    ser_->WriteRawBytes(data, len);
    return Just(true);
  }
  ValueSerializer* ser_ = nullptr;
};

class DeserDelegate : public ValueDeserializer::Delegate {
 public:
  MaybeLocal<Object> ReadHostObject(Isolate* isolate) override {
    uint32_t type = 0, len = 0;
    if (!des_->ReadUint32(&type) || !des_->ReadUint32(&len)) {
      isolate->ThrowError("go-bridge: host object header read failed");
      return MaybeLocal<Object>();
    }
    const void* bytes = nullptr;
    if (!des_->ReadRawBytes(len, &bytes)) {
      isolate->ThrowError("go-bridge: host object bytes read failed");
      return MaybeLocal<Object>();
    }
    return node::Buffer::Copy(isolate, static_cast<const char*>(bytes),
                              static_cast<size_t>(len));
  }
  ValueDeserializer* des_ = nullptr;
};

// Serialize any JS value to wire bytes (caller frees via free()).
static bool serialize_value(Isolate* isolate, Local<Context> ctx,
                            Local<Value> value, uint8_t** out, size_t* out_len) {
  SerDelegate del;
  ValueSerializer ser(isolate, &del);
  ser.SetTreatArrayBufferViewsAsHostObjects(true);
  del.ser_ = &ser;
  ser.WriteHeader();
  if (!ser.WriteValue(ctx, value).FromMaybe(false)) {
    return false;
  }
  auto pair = ser.Release();
  *out = pair.first;
  *out_len = pair.second;
  return true;
}

// Deserialize wire bytes to a JS value.
static MaybeLocal<Value> deserialize_value(Isolate* isolate, Local<Context> ctx,
                                           const uint8_t* data, size_t len) {
  DeserDelegate del;
  ValueDeserializer des(isolate, data, len, &del);
  del.des_ = &des;
  if (!des.ReadHeader(ctx).FromMaybe(false)) {
    return MaybeLocal<Value>();
  }
  return des.ReadValue(ctx);
}

// ----------------------------------------------------------------------
// connection
static void Connect(Isolate* isolate, const char* endpoint, const char* token) {
  pthread_mutex_lock(&g_io_mu);
  if (g_fd >= 0) teardown_connection();
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    pthread_mutex_unlock(&g_io_mu);
    isolate->ThrowError("go-bridge: socket failed");
    return;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, endpoint, sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    close(fd);
    pthread_mutex_unlock(&g_io_mu);
    isolate->ThrowError("go-bridge: connect failed");
    return;
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
    pthread_mutex_unlock(&g_io_mu);
    isolate->ThrowError("go-bridge: hello write failed");
    return;
  }
  struct timeval tv = {BRIDGE_TIMEOUT_SEC, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int bufsz = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  uint32_t rlen = 0;
  int err = read_frame_len(fd, &rlen);
  if (err == 0 && (rlen == 0 || rlen > 1024 * 1024)) err = EPROTO;
  char ready[256] = {0};
  if (err == 0 && rlen < sizeof(ready)) {
    err = read_full(fd, ready, rlen);
  }
  if (err != 0 || strstr(ready, "\"ready\"") == nullptr) {
    if (err != 0) {
      Warn("handshake failed (read)");
    } else {
      fprintf(stderr, "[go-bridge] ready got: %.80s\n", ready);
    }
    close(fd);
    pthread_mutex_unlock(&g_io_mu);
    isolate->ThrowError("go-bridge: handshake failed");
    return;
  }
  g_fd = fd;
  g_connected = 1;
  pthread_mutex_unlock(&g_io_mu);
}

// One synchronous v3 frame round trip. Input bytes are serialized outside
// the lock; the response bytes are copied into a std::vector under it.
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
  if (werr == 0) werr = write_all(g_fd, reinterpret_cast<const char*>(req), req_len);
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
// exports

static void NativeInitThunk(const FunctionCallbackInfo<Value>& args);

// preloadInit(electron): the entire preload surface that does not need the
// connection yet — the postMessage shim and the late-init hook.
static void PreloadInit(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsObject()) {
    isolate->ThrowError("preloadInit(electron) requires the electron module");
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  Local<Object> electron = args[0].As<Object>();
  Local<Value> ipc =
      electron->Get(ctx, S(isolate, "ipcRenderer")).ToLocalChecked();
  g_ipc_renderer.Reset(isolate, ipc.As<Object>());

  // window.chrome = window.chrome || {}; chrome.webview.postMessage shim
  Local<Object> global = ctx->Global();
  Local<Object> chrome;
  Local<Value> chrome_v =
      global->Get(ctx, S(isolate, "chrome")).ToLocalChecked();
  if (chrome_v->IsObject()) {
    chrome = chrome_v.As<Object>();
  } else {
    chrome = Object::New(isolate);
    global->Set(ctx, S(isolate, "chrome"), chrome).Check();
  }
  Local<Object> webview = Object::New(isolate);
  chrome->Set(ctx, S(isolate, "webview"), webview).Check();

  // postMessage shim: ipcRenderer.send('wails:message', msg)
  Local<FunctionTemplate> shim_tpl = FunctionTemplate::New(
      isolate,
      [](const FunctionCallbackInfo<Value>& args) {
        Isolate* iso = I(args);
        HandleScope hs(iso);
        Local<Context> c = iso->GetCurrentContext();
        Local<Object> ipc = g_ipc_renderer.Get(iso);
        if (ipc.IsEmpty() || args.Length() < 1) {
          return;
        }
        Local<Value> send_v =
            ipc->Get(c, S(iso, "send")).ToLocalChecked();
        Local<Function> send = send_v.As<Function>();
        Local<Value> argv[2] = {S(iso, "wails:message"), args[0]};
        Local<Value> ignored;
        if (!send->Call(c, ipc, 2, argv).ToLocal(&ignored)) {
          return;
        }
      });
  Local<Function> shim = shim_tpl->GetFunction(ctx).ToLocalChecked();
  webview->Set(ctx, S(isolate, "postMessage"), shim).Check();

  // the late-init hook main.js injects a call to (same signature as the
  // JS preload's): __wailsNativeInit({addon, endpoint, token})
  Local<FunctionTemplate> init_tpl =
      FunctionTemplate::New(isolate, NativeInitThunk);
  global->Set(ctx, S(isolate, "__wailsNativeInit"),
              init_tpl->GetFunction(ctx).ToLocalChecked())
      .Check();
}

// NativeInitThunk needs forward access; declare + define after FetchOverride.
static void NativeInitThunk(const FunctionCallbackInfo<Value>& args);

// FetchOverride(input, init): replaces window.fetch. Routes
// /wails/runtime requests over the UDS data plane; everything else goes to
// the original fetch.
static void FetchOverride(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();

  // fast bail: URL must name the runtime path
  bool native = g_connected != 0;
  const char* kMarker = "/wails/runtime";
  Local<Value> url_v = Undefined(isolate);
  if (native && args.Length() >= 1) {
    if (args[0]->IsString()) {
      url_v = args[0];
    } else if (args[0]->IsObject()) {
      // Request objects carry .url, URL objects .href — String(input)
      // semantics like the JS preload had
      Local<Object> o = args[0].As<Object>();
      Local<Value> u;
      if (o->Get(ctx, S(isolate, "url")).ToLocal(&u) && u->IsString()) {
        url_v = u;
      } else if (o->Get(ctx, S(isolate, "href")).ToLocal(&u) && u->IsString()) {
        url_v = u;
      } else if (args[0]->ToString(ctx).ToLocal(&u)) {
        url_v = u;
      }
    }
  }
  String::Utf8Value url(isolate, url_v);
  if (!native || *url == nullptr || strstr(*url, kMarker) == nullptr) {
    // original fetch carries on
    Local<Function> orig = g_orig_fetch.Get(isolate);
    if (orig.IsEmpty()) {
      isolate->ThrowError("go-bridge: fetch override not initialized");
      return;
    }
    int argc = args.Length();
    Local<Value> argv[2] = {args[0], args.Length() >= 2
                                        ? Local<Value>(args[1])
                                        : Local<Value>()};
    Local<Value> out =
        orig->Call(ctx, ctx->Global(), argc >= 2 ? 2 : 1, argv)
            .FromMaybe(Local<Value>());
    args.GetReturnValue().Set(out);
    return;
  }

  // extract init fields (all references — zero conversion)
  Local<Value> method, headers, body;
  if (args.Length() >= 2 && args[1]->IsObject()) {
    Local<Object> init = args[1].As<Object>();
    method = init->Get(ctx, S(isolate, "method")).FromMaybe(Local<Value>());
    headers = init->Get(ctx, S(isolate, "headers")).FromMaybe(Local<Value>());
    body = init->Get(ctx, S(isolate, "body")).FromMaybe(Local<Value>());
  }
  if (!method->IsString()) method = S(isolate, "GET");

  // build the call object — plain JS references, then one native write
  Local<Object> call = Object::New(isolate);
  call->Set(ctx, S(isolate, "channel"), S(isolate, "http")).Check();
  call->Set(ctx, S(isolate, "method"), method).Check();
  call->Set(ctx, S(isolate, "url"), url_v).Check();
  if (!body.IsEmpty() && !body->IsUndefined()) {
    call->Set(ctx, S(isolate, "body"), body).Check();
  } else {
    call->Set(ctx, S(isolate, "body"), S(isolate, "")).Check();
  }
  if (!headers.IsEmpty() && headers->IsObject()) {
    call->Set(ctx, S(isolate, "headers"), headers).Check();
  } else {
    call->Set(ctx, S(isolate, "headers"), Object::New(isolate)).Check();
  }

  uint8_t* req = nullptr;
  size_t req_len = 0;
  if (!serialize_value(isolate, ctx, call, &req, &req_len)) {
    fprintf(stderr, "[go-bridge] fetch: serialize failed\n");
    isolate->ThrowError("go-bridge: serialize call failed");
    return;
  }
  std::vector<uint8_t> resp;
  int err = frame_roundtrip(req, req_len, &resp);
  free(req);
  if (err != 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "go-bridge: invoke failed (%s)",
             strerror(err));
    isolate->ThrowError(msg);
    return;
  }

  Local<Value> out_v =
      deserialize_value(isolate, ctx, resp.data(), resp.size())
          .FromMaybe(Local<Value>());
  if (out_v.IsEmpty() || !out_v->IsObject()) {
    isolate->ThrowError("go-bridge: bad response frame");
    return;
  }
  Local<Object> out = out_v.As<Object>();

  // build the Response: new Response(bodyBuffer, {status, headers})
  int64_t status =
      out->Get(ctx, S(isolate, "status"))
          .FromMaybe(Local<Value>())
          .As<Number>()
          ->Value();
  Local<Value> ctype =
      out->Get(ctx, S(isolate, "contentType")).FromMaybe(Local<Value>());
  Local<Value> body_v =
      out->Get(ctx, S(isolate, "body")).FromMaybe(Local<Value>());

  Local<Object> resp_init = Object::New(isolate);
  resp_init->Set(ctx, S(isolate, "status"), Integer::New(isolate, static_cast<int32_t>(status))).Check();
  Local<Object> rh = Object::New(isolate);
  if (ctype->IsString() && ctype.As<String>()->Length() > 0) {
    rh->Set(ctx, S(isolate, "Content-Type"), ctype).Check();
  }
  resp_init->Set(ctx, S(isolate, "headers"), rh).Check();

  int argc = 2;
  Local<Value> argv[2];
  if (body_v->IsArrayBufferView()) {
    argv[0] = body_v;
  } else if (body_v->IsString()) {
    argv[0] = body_v;
  } else {
    argv[0] = Null(isolate);
  }
  argv[1] = resp_init;
  Local<Function> resp_ctor = g_response_ctor.Get(isolate);
  Local<Value> response =
      resp_ctor->NewInstance(ctx, argc, argv).FromMaybe(Local<Value>());
  if (response.IsEmpty()) {
    return;  // exception already pending
  }
  args.GetReturnValue().Set(response);
}

// __nativeEcho(payload) — sync {channel:"echo"} round trip, payload back.
static void NativeEcho(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  if (!g_connected) {
    isolate->ThrowError("go-bridge: not connected");
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  Local<Object> msg = Object::New(isolate);
  msg->Set(ctx, S(isolate, "id"), Integer::New(isolate, 0)).Check();
  msg->Set(ctx, S(isolate, "channel"), S(isolate, "echo")).Check();
  msg->Set(ctx, S(isolate, "payload"),
           args.Length() >= 1 ? Local<Value>(args[0]) : Undefined(isolate))
      .Check();
  uint8_t* req = nullptr;
  size_t req_len = 0;
  if (!serialize_value(isolate, ctx, msg, &req, &req_len)) {
    isolate->ThrowError("go-bridge: serialize echo failed");
    return;
  }
  std::vector<uint8_t> resp;
  int err = frame_roundtrip(req, req_len, &resp);
  free(req);
  if (err != 0) {
    char m[128];
    snprintf(m, sizeof(m), "go-bridge: echo failed (%s)", strerror(err));
    isolate->ThrowError(m);
    return;
  }
  Local<Value> out_v =
      deserialize_value(isolate, ctx, resp.data(), resp.size())
          .FromMaybe(Local<Value>());
  if (out_v.IsEmpty() || !out_v->IsObject()) {
    isolate->ThrowError("go-bridge: bad echo response");
    return;
  }
  Local<Value> payload;
  if (!out_v.As<Object>()->Get(ctx, S(isolate, "payload")).ToLocal(&payload)) {
    payload = Undefined(isolate);
  }
  args.GetReturnValue().Set(payload);
}

// __nativeInvoke(msgObj) — generic single-frame invoke returning the
// response object (kept for parity with the JS preload's export).
static void NativeInvoke(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  if (!g_connected) {
    isolate->ThrowError("go-bridge: not connected");
    return;
  }
  if (args.Length() < 1 || !args[0]->IsObject()) {
    isolate->ThrowError("go-bridge: invoke(msgObj) requires an object");
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  uint8_t* req = nullptr;
  size_t req_len = 0;
  if (!serialize_value(isolate, ctx, args[0], &req, &req_len)) {
    isolate->ThrowError("go-bridge: serialize failed");
    return;
  }
  std::vector<uint8_t> resp;
  int err = frame_roundtrip(req, req_len, &resp);
  free(req);
  if (err != 0) {
    char m[128];
    snprintf(m, sizeof(m), "go-bridge: invoke failed (%s)", strerror(err));
    isolate->ThrowError(m);
    return;
  }
  Local<Value> out =
      deserialize_value(isolate, ctx, resp.data(), resp.size())
          .FromMaybe(Local<Value>());
  if (out.IsEmpty()) {
    isolate->ThrowError("go-bridge: bad response frame");
    return;
  }
  args.GetReturnValue().Set(out);
}

// __wailsV8Serialize(value) -> Buffer (node-compatible bytes)
static void V8Serialize(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  if (args.Length() < 1) {
    isolate->ThrowError("v8Serialize(value) requires 1 arg");
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  uint8_t* bytes = nullptr;
  size_t len = 0;
  if (!serialize_value(isolate, ctx, args[0], &bytes, &len)) {
    isolate->ThrowError("go-bridge: serialize failed");
    return;
  }
  Local<Object> buf = node::Buffer::Copy(
      isolate, reinterpret_cast<const char*>(bytes), len).ToLocalChecked();
  free(bytes);
  args.GetReturnValue().Set(buf);
}

// __wailsV8Deserialize(Buffer|TypedArray) -> value
static void V8Deserialize(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsArrayBufferView()) {
    isolate->ThrowError("v8Deserialize(bytes) requires a buffer");
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  auto view = args[0].As<ArrayBufferView>();
  std::shared_ptr<BackingStore> bs = view->Buffer()->GetBackingStore();
  const uint8_t* data =
      reinterpret_cast<const uint8_t*>(bs->Data()) + view->ByteOffset();
  Local<Value> out = deserialize_value(isolate, ctx, data, view->ByteLength())
                         .FromMaybe(Local<Value>());
  if (out.IsEmpty()) {
    isolate->ThrowError("go-bridge: deserialize failed");
    return;
  }
  args.GetReturnValue().Set(out);
}

static void Close(const FunctionCallbackInfo<Value>& args) {
  pthread_mutex_lock(&g_io_mu);
  teardown_connection();
  pthread_mutex_unlock(&g_io_mu);
}

// __wailsNativeInit({endpoint, token, addon}) — connect + flags + fetch
// override. Same entry main.js's executeJavaScript injection calls.
static void NativeInitThunk(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope scope(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsObject()) {
    isolate->ThrowError("__wailsNativeInit(config) requires an object");
    return;
  }
  Local<Object> cfg = args[0].As<Object>();
  String::Utf8Value endpoint(
      isolate, cfg->Get(ctx, S(isolate, "endpoint")).FromMaybe(Local<Value>()));
  String::Utf8Value token(
      isolate, cfg->Get(ctx, S(isolate, "token")).FromMaybe(Local<Value>()));
  if (*endpoint == nullptr || *token == nullptr) {
    isolate->ThrowError("go-bridge: config missing endpoint/token");
    return;
  }
  Connect(isolate, *endpoint, *token);
  if (!g_connected) {
    return;  // exception already pending from Connect
  }

  Local<Object> global = ctx->Global();
  // resolve the Response constructor once (Function used for NewInstance)
  Local<Value> response =
      global->Get(ctx, S(isolate, "Response")).FromMaybe(Local<Value>());
  if (response->IsFunction()) {
    g_response_ctor.Reset(isolate, response.As<Function>());
  }
  // runtime.js gates its call-body encoding on these (set only after the
  // handshake, so a failed connect leaves the plain-HTTP fallback intact)
  global->Set(ctx, S(isolate, "__wailsV8Body"), Boolean::New(isolate, true)).Check();
  Local<FunctionTemplate> ser_t =
      FunctionTemplate::New(isolate, V8Serialize);
  Local<FunctionTemplate> deser_t =
      FunctionTemplate::New(isolate, V8Deserialize);
  Local<Function> ser = ser_t->GetFunction(ctx).ToLocalChecked();
  Local<Function> deser = deser_t->GetFunction(ctx).ToLocalChecked();
  global->Set(ctx, S(isolate, "__wailsV8Serialize"), ser).Check();
  global->Set(ctx, S(isolate, "__wailsV8Deserialize"), deser).Check();

  Local<FunctionTemplate> inv_t = FunctionTemplate::New(isolate, NativeInvoke);
  Local<FunctionTemplate> echo_t = FunctionTemplate::New(isolate, NativeEcho);
  Local<Function> inv = inv_t->GetFunction(ctx).ToLocalChecked();
  Local<Function> echo = echo_t->GetFunction(ctx).ToLocalChecked();
  global->Set(ctx, S(isolate, "__nativeInvoke"), inv).Check();
  global->Set(ctx, S(isolate, "__nativeEcho"), echo).Check();
  // legacy existence gate (index.html waits on it before benching)
  global->Set(ctx, S(isolate, "__nativeCall"), echo).Check();

  // install the fetch override, keeping the original
  Local<Value> orig =
      global->Get(ctx, S(isolate, "fetch")).FromMaybe(Local<Value>());
  g_orig_fetch.Reset(isolate, orig.As<Function>());
  Local<FunctionTemplate> fetch_t =
      FunctionTemplate::New(isolate, FetchOverride);
  global->Set(ctx, S(isolate, "fetch"), fetch_t->GetFunction(ctx).ToLocalChecked()).Check();
  global->Set(ctx, S(isolate, "__nativeHttpActive"), Boolean::New(isolate, true)).Check();

  fprintf(stderr, "[go-bridge] native-uds transport ready\n");
}

// NODE_MODULE_INIT = context-aware registration (required for renderer
// loading in Electron; the N-API addon was context-aware automatically).
NODE_MODULE_INIT(/* exports, module, context */) {
  NODE_SET_METHOD(exports, "preloadInit", PreloadInit);
  NODE_SET_METHOD(exports, "close", Close);
}
