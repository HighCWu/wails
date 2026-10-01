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

#include <dlfcn.h>

#include <cerrno>
#include <cstdarg>
#include <ctime>
#include <map>
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
static Global<Object> g_webutils;
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
static void PostMessageAdditionalCb(const FunctionCallbackInfo<Value>& args);
static Local<Value> JsonStringify(Isolate* isolate, Local<Context> ctx, Local<Value> v);

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
  // WebView2 file-drop contract (see PostMessageAdditionalCb) + webUtils
  Local<Value> wu;
  if (electron->Get(ctx, S(isolate, "webUtils")).ToLocal(&wu) && wu->IsObject()) {
    g_webutils.Reset(isolate, wu.As<Object>());
  }
  Local<FunctionTemplate> pma_tpl = FunctionTemplate::New(isolate, PostMessageAdditionalCb);
  webview->Set(ctx, S(isolate, "postMessageWithAdditionalObjects"),
               pma_tpl->GetFunction(ctx).ToLocalChecked())
      .Check();

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
static void PostMessageAdditionalCb(const FunctionCallbackInfo<Value>& args);
static Local<Value> JsonStringify(Isolate* isolate, Local<Context> ctx, Local<Value> v);

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

// postMessageWithAdditionalObjects("file:drop:<x>:<y>", files): the
// WebView2 file-drop contract the Wails runtime probes for. On Electron we
// resolve real paths with webUtils.getPathForFile and forward them as a
// wails:file-drop message; the host maps the sender to the window and
// feeds the same dragAndDrop pipeline (Window.OnFileDrop works unchanged).
static void PostMessageAdditionalCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsArray()) return;
  String::Utf8Value msg(isolate, args[0]);
  if (*msg == nullptr || strncmp(*msg, "file:drop:", 10) != 0) return;
  int x = 0, y = 0;
  sscanf(*msg + 10, "%d:%d", &x, &y);
  Local<Object> webutils = g_webutils.Get(isolate);
  Local<Function> getpath;
  if (!webutils.IsEmpty()) {
    Local<Value> v;
    if (webutils->Get(ctx, S(isolate, "getPathForFile")).ToLocal(&v) &&
        v->IsFunction()) {
      getpath = v.As<Function>();
    }
  }
  Local<Array> files = args[1].As<Array>();
  uint32_t n = files->Length();
  Local<Array> names = Array::New(isolate, n);
  uint32_t out = 0;
  for (uint32_t i = 0; i < n; i++) {
    Local<Value> f;
    if (!files->Get(ctx, Integer::NewFromUnsigned(isolate, i)).ToLocal(&f) ||
        !f->IsObject() || getpath.IsEmpty()) {
      continue;
    }
    Local<Value> argv[1] = {f};
    Local<Value> path = getpath->Call(ctx, webutils, 1, argv).FromMaybe(Local<Value>());
    if (!path.IsEmpty() && path->IsString() && path.As<String>()->Length() > 0) {
      names->Set(ctx, Integer::NewFromUnsigned(isolate, out++), path).Check();
    }
  }
  Local<Object> payload = Object::New(isolate);
  payload->Set(ctx, S(isolate, "x"), Integer::New(isolate, x)).Check();
  payload->Set(ctx, S(isolate, "y"), Integer::New(isolate, y)).Check();
  payload->Set(ctx, S(isolate, "filenames"), names).Check();
  Local<Value> json = JsonStringify(isolate, ctx, payload);
  if (json.IsEmpty() || !json->IsString()) return;
  Local<Object> ipc = g_ipc_renderer.Get(isolate);
  if (ipc.IsEmpty()) return;
  Local<Value> sargv[2] = {
      S(isolate, "wails:message"),
      String::Concat(isolate, S(isolate, "wails:file-drop:"), json.As<String>())};
  Local<Value> ignored;
  Local<Value> send;
  if (ipc->Get(ctx, S(isolate, "send")).ToLocal(&send) && send->IsFunction()) {
    send.As<Function>()->Call(ctx, ipc, 2, sargv).ToLocal(&ignored);
  }
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

static void x11_move_resize_legacy(unsigned long xid, int direction) {
  if (!x11_ready()) {
    fprintf(stderr, "[go-bridge] x11_move_resize: X11 init failed\n");
    return;
  }
  fprintf(stderr, "[go-bridge] x11_move_resize xid=%lu dir=%d\n", xid, direction);
  pthread_mutex_lock(&g_x_mu);
  auto root_of = reinterpret_cast<unsigned long (*)(void*)>(dlsym(g_x11_so, "XDefaultRootWindow"));
  auto intern_atom = reinterpret_cast<unsigned long (*)(void*, const char*, int)>(dlsym(g_x11_so, "XInternAtom"));
  auto send_event = reinterpret_cast<int (*)(void*, unsigned long, int, long, void*)>(dlsym(g_x11_so, "XSendEvent"));
  auto flush = reinterpret_cast<int (*)(void*)>(dlsym(g_x11_so, "XFlush"));
  auto query_pointer = reinterpret_cast<int (*)(void*, unsigned long, unsigned long*, unsigned long*, int*, int*, int*, int*, unsigned*)>(dlsym(g_x11_so, "XQueryPointer"));
  if (root_of == nullptr || intern_atom == nullptr || send_event == nullptr || flush == nullptr ||
      query_pointer == nullptr) {
    pthread_mutex_unlock(&g_x_mu);
    return;
  }
  unsigned long root = root_of(g_xdisp);
  unsigned long child = 0;
  int rx = 0, ry = 0, wx = 0, wy = 0;
  unsigned mask = 0;
  query_pointer(g_xdisp, root, &root, &child, &rx, &ry, &wx, &wy, &mask);
  XClientMsgEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = 33;  // ClientMessage
  ev.window = xid;
  ev.message_type = g_wmmove_atom;
  ev.format = 32;
  ev.data[0] = xid;
  ev.data[1] = g_wmmove_atom;
  ev.data[2] = static_cast<unsigned long>(direction);
  ev.data[3] = static_cast<unsigned long>(rx);
  ev.data[4] = static_cast<unsigned long>(ry);
  ev.data[5] = 1;  // source indication: application
  unsigned long buf[24];  // XEvent-sized scratch; XSendEvent reads by type
  memset(buf, 0, sizeof(buf));
  memcpy(buf, &ev, sizeof(ev));
  int sent = send_event(g_xdisp, root, 0, (1L << 20) | (1L << 19), buf);  // SubstructureRedirect|Notify
  flush(g_xdisp);
  fprintf(stderr, "[go-bridge] x11 send: atom=%lu root=%lu rx=%d ry=%d sent=%d\n",
          g_wmmove_atom, root, rx, ry, sent);
  pthread_mutex_unlock(&g_x_mu);
}

// ======================================================================
// main-process surface (stage 3): window management, the stdio control
// protocol, and event forwarding — the entire former main.js, native.
//
//   request : {"t":"req","id":N,"m":"<method>","p":{...,"id":windowID}}
//   response: {"t":"resp","id":N,"ok":true,"r":...} | {"t":"resp",ok:false,err}
//   event   : {"t":"ev","e":"<name>","p":{...,"id":windowID}}
// Quitting: stdin EOF means the host is gone (orphan protection layer 1);
// a PPID poll covers SIGKILL'd hosts (layer 2).
#include <uv.h>

static Global<Object> g_app, g_winctor, g_ipcmain;
static bool g_m_debug = false;
static std::string g_cfg_preload, g_cfg_bridge_path, g_cfg_bridge_token,
    g_cfg_native_addon;
static uint32_t g_orig_ppid = 0;
static uv_loop_t* g_loop = nullptr;
static uv_async_t g_lines_async;
static uv_async_t g_quit_async;
static std::mutex g_lines_mu;
static std::vector<std::string> g_lines;
static bool g_lines_eof = false;
static std::mutex g_out_mu;
static Isolate* g_main_isolate = nullptr;
static std::map<uint32_t, Global<Object>> g_windows;
static std::map<int32_t, uint32_t> g_by_wc;

// JSON via the global JS object, NOT v8::JSON::*: Electron builds V8
// against libc++, so v8::JSON::Parse's std::optional parameter mangles
// differently under g++/libstdc++ and the symbol never resolves. Calling
// the same engine's JSON global through JS has no std:: types on the wire.
static Local<Value> JsonParse(Isolate* isolate, Local<Context> ctx,
                              Local<String> s) {
  Local<Object> j = ctx->Global()
                        ->Get(ctx, S(isolate, "JSON"))
                        .ToLocalChecked()
                        .As<Object>();
  Local<Function> parse = j->Get(ctx, S(isolate, "parse"))
                              .ToLocalChecked()
                              .As<Function>();
  Local<Value> argv[1] = {s};
  return parse->Call(ctx, j, 1, argv).FromMaybe(Local<Value>());
}

static Local<Value> JsonStringify(Isolate* isolate, Local<Context> ctx,
                                  Local<Value> v) {
  Local<Object> j = ctx->Global()
                        ->Get(ctx, S(isolate, "JSON"))
                        .ToLocalChecked()
                        .As<Object>();
  Local<Function> stringify = j->Get(ctx, S(isolate, "stringify"))
                                  .ToLocalChecked()
                                  .As<Function>();
  Local<Value> argv[1] = {v};
  return stringify->Call(ctx, j, 1, argv).FromMaybe(Local<Value>());
}

static void ELog(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[wails-electron] ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

// JSON object -> stdout line (the control channel to Go).
static void SendObj(Isolate* isolate, Local<Context> ctx, Local<Object> obj) {
  Local<Value> sv = JsonStringify(isolate, ctx, obj);
  if (sv.IsEmpty() || !sv->IsString()) return;
  Local<String> s = sv.As<String>();
  String::Utf8Value u(isolate, s);
  std::lock_guard<std::mutex> lk(g_out_mu);
  std::string line(*u, u.length());
  line += '\n';
  if (write_all(1, line.data(), line.size()) != 0 && g_m_debug) {
    ELog("send failed");
  }
}

static Local<Object> NewEv(Isolate* isolate, Local<Context> ctx,
                           const char* ev, uint32_t id) {
  Local<Object> o = Object::New(isolate);
  o->Set(ctx, S(isolate, "t"), S(isolate, "ev")).Check();
  o->Set(ctx, S(isolate, "e"), S(isolate, ev)).Check();
  Local<Object> p = Object::New(isolate);
  p->Set(ctx, S(isolate, "id"), Integer::NewFromUnsigned(isolate, id)).Check();
  o->Set(ctx, S(isolate, "p"), p).Check();
  return o;
}

static void WinEvent(Isolate* isolate, Local<Context> ctx, uint32_t id,
                     const char* ev) {
  SendObj(isolate, ctx, NewEv(isolate, ctx, ev, id));
}

// extra: an object whose own properties are merged into the event payload.
static void WinEventExtra(Isolate* isolate, Local<Context> ctx, uint32_t id,
                          const char* ev, Local<Object> extra) {
  Local<Object> o = NewEv(isolate, ctx, ev, id);
  Local<Object> p =
      o->Get(ctx, S(isolate, "p")).ToLocalChecked().As<Object>();
  Local<Array> names =
      extra->GetOwnPropertyNames(ctx).ToLocalChecked();
  uint32_t n = names->Length();
  for (uint32_t i = 0; i < n; i++) {
    Local<Value> k = names->Get(ctx, Integer::NewFromUnsigned(isolate, i))
                         .ToLocalChecked();
    Local<Value> v = extra->Get(ctx, k).ToLocalChecked();
    p->Set(ctx, k, v).Check();
  }
  SendObj(isolate, ctx, o);
}

static void SendResp(Isolate* isolate, Local<Context> ctx, int64_t id,
                     Local<Value> result) {
  Local<Object> o = Object::New(isolate);
  o->Set(ctx, S(isolate, "t"), S(isolate, "resp")).Check();
  o->Set(ctx, S(isolate, "id"), Number::New(isolate, static_cast<double>(id)))
      .Check();
  o->Set(ctx, S(isolate, "ok"), Boolean::New(isolate, true)).Check();
  o->Set(ctx, S(isolate, "r"),
         result->IsUndefined() ? Null(isolate) : result)
      .Check();
  SendObj(isolate, ctx, o);
}

static void SendRespErr(Isolate* isolate, Local<Context> ctx, int64_t id,
                        const char* err) {
  Local<Object> o = Object::New(isolate);
  o->Set(ctx, S(isolate, "t"), S(isolate, "resp")).Check();
  o->Set(ctx, S(isolate, "id"), Number::New(isolate, static_cast<double>(id)))
      .Check();
  o->Set(ctx, S(isolate, "ok"), Boolean::New(isolate, false)).Check();
  o->Set(ctx, S(isolate, "err"), S(isolate, err)).Check();
  SendObj(isolate, ctx, o);
}

// ---- param readers (JSON-shaped, JS-truthiness for booleans) ----
static double PNum(Isolate* isolate, Local<Context> ctx, Local<Object> p,
                   const char* k, double dflt) {
  Local<Value> v;
  if (!p->Get(ctx, S(isolate, k)).ToLocal(&v) || !v->IsNumber()) return dflt;
  return v.As<Number>()->Value();
}
static std::string PStr(Isolate* isolate, Local<Context> ctx, Local<Object> p,
                        const char* k) {
  Local<Value> v;
  if (!p->Get(ctx, S(isolate, k)).ToLocal(&v) || !v->IsString())
    return std::string();
  String::Utf8Value u(isolate, v);
  return std::string(*u, u.length());
}
static bool PBool(Isolate* isolate, Local<Context> ctx, Local<Object> p,
                  const char* k) {
  Local<Value> v;
  if (!p->Get(ctx, S(isolate, k)).ToLocal(&v)) return false;
  if (v->IsBoolean()) return v->IsTrue();
  if (v->IsNullOrUndefined()) return false;
  if (v->IsNumber()) return v.As<Number>()->Value() != 0;
  if (v->IsString()) return v.As<String>()->Length() > 0;
  return true;
}

static Local<Object> GetWin(Isolate* isolate, Local<Context> ctx,
                            Local<Object> p) {
  uint32_t id = static_cast<uint32_t>(PNum(isolate, ctx, p, "id", -1));
  auto it = g_windows.find(id);
  if (it == g_windows.end()) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Error: unknown window %u", id);
    isolate->ThrowError(msg);
    return Local<Object>();
  }
  return it->second.Get(isolate);
}

// Call a no-arg method on the window (or webContents) and return its value.
static Local<Value> CallWin(Isolate* isolate, Local<Context> ctx,
                            Local<Object> win, const char* obj_key,
                            const char* method) {
  Local<Object> target = win;
  if (obj_key != nullptr) {
    Local<Value> t;
    if (!win->Get(ctx, S(isolate, obj_key)).ToLocal(&t) || !t->IsObject()) {
      return Local<Value>();
    }
    target = t.As<Object>();
  }
  Local<Value> fv;
  if (!target->Get(ctx, S(isolate, method)).ToLocal(&fv) ||
      !fv->IsFunction()) {
    isolate->ThrowError("missing method");
    return Local<Value>();
  }
  Local<Function> fn = fv.As<Function>();
  return fn.As<Function>()
      ->Call(ctx, target, 0, nullptr)
      .FromMaybe(Local<Value>());
}

static Local<Value> CallWin1(Isolate* isolate, Local<Context> ctx,
                             Local<Object> win, const char* obj_key,
                             const char* method, Local<Value> a) {
  Local<Object> target = win;
  if (obj_key != nullptr) {
    Local<Value> t;
    if (!win->Get(ctx, S(isolate, obj_key)).ToLocal(&t) || !t->IsObject()) {
      return Local<Value>();
    }
    target = t.As<Object>();
  }
  Local<Value> fv;
  if (!target->Get(ctx, S(isolate, method)).ToLocal(&fv) ||
      !fv->IsFunction()) {
    isolate->ThrowError("missing method");
    return Local<Value>();
  }
  Local<Function> fn = fv.As<Function>();
  return fn.As<Function>()
      ->Call(ctx, target, 1, &a)
      .FromMaybe(Local<Value>());
}

// ---- promise-aware respond: mirror of main.js respond() ----
static void ResolvedCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  int64_t id = reinterpret_cast<int64_t>(args.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
  Local<Context> ctx = isolate->GetCurrentContext();
  SendResp(isolate, ctx, id, args.Length() >= 1 ? Local<Value>(args[0])
                                                : Undefined(isolate));
}
static void RejectedCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  int64_t id = reinterpret_cast<int64_t>(args.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
  Local<Context> ctx = isolate->GetCurrentContext();
  Local<String> s;
  std::string err = "Error";
  if (args.Length() >= 1 &&
      args[0]->ToString(ctx).ToLocal(&s)) {
    String::Utf8Value u(isolate, s);
    err.assign(*u, u.length());
  }
  SendRespErr(isolate, ctx, id, err.c_str());
}

static void RespondValue(Isolate* isolate, Local<Context> ctx, int64_t id,
                         Local<Value> result) {
  if (result.IsEmpty()) return;  // exception path handled by caller
  if (result->IsPromise()) {
    int64_t* box = new int64_t(id);
    Local<External> data = External::New(isolate, box, kExternalPointerTypeTagDefault);
    Local<Function> ok =
        FunctionTemplate::New(isolate, ResolvedCb, data)
            ->GetFunction(ctx).ToLocalChecked();
    Local<Function> bad =
        FunctionTemplate::New(isolate, RejectedCb, data)
            ->GetFunction(ctx).ToLocalChecked();
    result.As<Promise>()->Then(ctx, ok, bad).FromMaybe(Local<Promise>());
    return;
  }
  SendResp(isolate, ctx, id, result);
}

// ---- window event wiring ----
struct EvCtx {
  uint32_t id;
  int32_t wc;
  const char* ev;      // event name reported to Go
  bool extra_bounds;   // attach win.getBounds() to the payload
  bool file_drop;      // runtime flags injected on did-finish-load
  bool frameless;
};

static void WindowEventCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  EvCtx* ec = reinterpret_cast<EvCtx*>(
      args.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (strcmp(ec->ev, "closed") == 0) {
    WinEvent(isolate, ctx, ec->id, "closed");
    g_windows.erase(ec->id);
    g_by_wc.erase(ec->wc);
    return;
  }
  if (ec->extra_bounds) {
    Local<Object> win = g_windows[ec->id].Get(isolate);
    if (win.IsEmpty()) {
      WinEvent(isolate, ctx, ec->id, ec->ev);
      return;
    }
    Local<Value> bounds =
        CallWin(isolate, ctx, win, nullptr, "getBounds");
    if (!bounds.IsEmpty() && bounds->IsObject()) {
      WinEventExtra(isolate, ctx, ec->id, ec->ev, bounds.As<Object>());
      return;
    }
  }
  WinEvent(isolate, ctx, ec->id, ec->ev);
}

static void ConsoleMessageCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  if (args.Length() < 3 || !args[2]->IsString()) return;
  String::Utf8Value msg(isolate, args[2]);
  fprintf(stderr, "[renderer] %s\n", *msg);
}

static void RenderGoneCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  EvCtx* ec = reinterpret_cast<EvCtx*>(
      args.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
  std::string reason;
  if (args.Length() >= 2 && args[1]->IsObject()) {
    Local<Value> r;
    if (args[1].As<Object>()->Get(ctx, S(isolate, "reason")).ToLocal(&r) &&
        r->IsString()) {
      String::Utf8Value u(isolate, r);
      reason.assign(*u, u.length());
    }
  }
  Local<Object> extra = Object::New(isolate);
  extra->Set(ctx, S(isolate, "reason"), S(isolate, reason.c_str())).Check();
  WinEventExtra(isolate, ctx, ec->id, "render-gone", extra);
}

// did-finish-load: the renderer probe + native-transport injection.
static void InjectDoneOkCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  std::string extra;
  Local<String> ps;
  if (args.Length() >= 1 && args[0]->ToString(ctx).ToLocal(&ps)) {
    String::Utf8Value u(isolate, ps);
    extra.assign(*u, u.length());
  }
  ELog("native init injected ok %s", extra.c_str());  // reports the dnd shim state
}
static void InjectDoneErrCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  Local<String> s;
  std::string e = "?";
  if (args.Length() >= 1 && args[0]->ToString(ctx).ToLocal(&s)) {
    String::Utf8Value u(isolate, s);
    e.assign(*u, u.length());
  }
  ELog("native init inject failed: %s", e.c_str());
}
static void ProbeOkCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  std::string probe = "<no result>";
  Local<Value> probe_v = args.Length() >= 1 ? Local<Value>(args[0]) : Local<Value>();
  if (!probe_v.IsEmpty() && probe_v->IsObject()) {
    probe_v = JsonStringify(isolate, ctx, probe_v);
  }
  Local<String> ps;
  if (!probe_v.IsEmpty() && probe_v->ToString(ctx).ToLocal(&ps)) {
    String::Utf8Value u(isolate, ps);
    probe.assign(*u, u.length());
  }
  ELog("renderer probe: %s", probe.c_str());
  if (g_cfg_native_addon.empty() || g_cfg_bridge_path.empty()) return;
  EvCtx* ec = reinterpret_cast<EvCtx*>(
      args.Data().As<External>()->Value(v8::kExternalPointerTypeTagDefault));
  // rebuild the config JSON safely via the engine's own stringify
  Local<Object> cfg = Object::New(isolate);
  cfg->Set(ctx, S(isolate, "addon"), S(isolate, g_cfg_native_addon.c_str()))
      .Check();
  cfg->Set(ctx, S(isolate, "endpoint"), S(isolate, g_cfg_bridge_path.c_str()))
      .Check();
  cfg->Set(ctx, S(isolate, "token"), S(isolate, g_cfg_bridge_token.c_str()))
      .Check();
  Local<Value> cfg_json = JsonStringify(isolate, ctx, cfg);
  if (cfg_json.IsEmpty() || !cfg_json->IsString()) return;
  String::Utf8Value cj(isolate, cfg_json);
  std::string script =
      "window._wails=window._wails||{};window._wails.flags=window._wails.flags||{};";
  script += "window._wails.flags.enableFileDrop=";
  script += ec->file_drop ? "true" : "false";
  script += ";window._wails.flags.frameless=";
  script += ec->frameless ? "true" : "false";
  script += ";typeof __wailsNativeInit === 'function' && __wailsNativeInit(";
  script += *cj;
  script += ")";
  script += ";'dnd=' + typeof window.chrome.webview.postMessageWithAdditionalObjects";
  Local<Value> unused;
  if (!args.This()
               ->Get(ctx, S(isolate, "executeJavaScript"))
               .ToLocal(&unused))
    return;
  Local<Function> exec = unused.As<Function>();
  Local<Value> argv[1] = {S(isolate, script.c_str())};
  Local<Value> prom;
  if (!exec->Call(ctx, args.This(), 1, argv).ToLocal(&prom)) {
    ELog("probe executeJavaScript failed: inject call");
    return;
  }
  if (prom->IsPromise()) {
    int64_t* box = new int64_t(ec->id);
    Local<External> data = External::New(isolate, box, kExternalPointerTypeTagDefault);
    Local<Function> ok = FunctionTemplate::New(isolate, InjectDoneOkCb, data)
                             ->GetFunction(ctx).ToLocalChecked();
    Local<Function> bad = FunctionTemplate::New(isolate, InjectDoneErrCb, data)
                              ->GetFunction(ctx).ToLocalChecked();
    prom.As<Promise>()->Then(ctx, ok, bad).FromMaybe(Local<Promise>());
  }
}
static void ProbeErrCb(const FunctionCallbackInfo<Value>& args) {
  ELog("probe executeJavaScript failed: probe promise");
}

static void WireWindow(Isolate* isolate, Local<Context> ctx, uint32_t id,
                       int32_t wc, Local<Object> win,
                       Local<Object> webcontents, bool file_drop,
                       bool frameless) {
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
    EvCtx* ec = new EvCtx{id, wc, e.go, e.bounds, false, false};
    Local<Function> fn =
        FunctionTemplate::New(isolate, WindowEventCb, External::New(isolate, ec, kExternalPointerTypeTagDefault))
            ->GetFunction(ctx).ToLocalChecked();
    Local<Value> argv[2] = {S(isolate, e.dom), fn};
    Local<Value> ignored;
    win->Get(ctx, S(isolate, "on"))
        .ToLocalChecked()
        .As<Function>()
        ->Call(ctx, win, 2, argv)
        .ToLocal(&ignored);
  }
  // renderer observability
  {
    Local<Function> fn =
        FunctionTemplate::New(isolate, ConsoleMessageCb)
            ->GetFunction(ctx).ToLocalChecked();
    Local<Value> argv[2] = {S(isolate, "console-message"), fn};
    Local<Value> ignored;
    webcontents->Get(ctx, S(isolate, "on"))
        .ToLocalChecked()
        .As<Function>()
        ->Call(ctx, webcontents, 2, argv)
        .ToLocal(&ignored);
  }
  {
    EvCtx* ec = new EvCtx{id, wc, "render-gone", false, false, false};
    Local<Function> fn =
        FunctionTemplate::New(isolate, RenderGoneCb,
                              External::New(isolate, ec, kExternalPointerTypeTagDefault))
            ->GetFunction(ctx).ToLocalChecked();
    Local<Value> argv[2] = {S(isolate, "render-process-gone"), fn};
    Local<Value> ignored;
    webcontents->Get(ctx, S(isolate, "on"))
        .ToLocalChecked()
        .As<Function>()
        ->Call(ctx, webcontents, 2, argv)
        .ToLocal(&ignored);
  }
  // did-finish-load: probe + inject
  {
    EvCtx* ec = new EvCtx{id, wc, "did-finish-load", false, file_drop, frameless};
    Local<Function> fn =
        FunctionTemplate::New(isolate, ProbeOkCb,
                              External::New(isolate, ec, kExternalPointerTypeTagDefault))
            ->GetFunction(ctx).ToLocalChecked();
    Local<Value> argv[2] = {S(isolate, "did-finish-load"), fn};
    Local<Value> ignored;
    webcontents->Get(ctx, S(isolate, "on"))
        .ToLocalChecked()
        .As<Function>()
        ->Call(ctx, webcontents, 2, argv)
        .ToLocal(&ignored);
  }
}

static Local<Value> CreateWindow(Isolate* isolate, Local<Context> ctx,
                                 Local<Object> p) {
  Local<Object> opts = Object::New(isolate);
  double x = PNum(isolate, ctx, p, "x", 0), y = PNum(isolate, ctx, p, "y", 0);
  Local<Value> xv = p->Get(ctx, S(isolate, "x")).ToLocalChecked();
  Local<Value> yv = p->Get(ctx, S(isolate, "y")).ToLocalChecked();
  bool has_x = !xv->IsUndefined();
  bool has_y = !yv->IsUndefined();
  if (has_x) opts->Set(ctx, S(isolate, "x"), Number::New(isolate, x)).Check();
  if (has_y) opts->Set(ctx, S(isolate, "y"), Number::New(isolate, y)).Check();
  double w = PNum(isolate, ctx, p, "width", 800);
  double h = PNum(isolate, ctx, p, "height", 600);
  opts->Set(ctx, S(isolate, "width"), Number::New(isolate, w)).Check();
  opts->Set(ctx, S(isolate, "height"), Number::New(isolate, h)).Check();
  std::string title = PStr(isolate, ctx, p, "title");
  opts->Set(ctx, S(isolate, "title"), S(isolate, title.c_str())).Check();
  bool frameless = PBool(isolate, ctx, p, "frameless");
  opts->Set(ctx, S(isolate, "frame"), Boolean::New(isolate, !frameless)).Check();
  bool transparent = PBool(isolate, ctx, p, "transparent");
  opts->Set(ctx, S(isolate, "transparent"), Boolean::New(isolate, transparent))
      .Check();
  // JS: resizable: p.resizable !== false  (undefined -> true)
  bool resizable = true;
  {
    Local<Value> rv = p->Get(ctx, S(isolate, "resizable")).ToLocalChecked();
    if (rv->IsBoolean() && !rv->IsTrue()) resizable = false;
  }
  opts->Set(ctx, S(isolate, "resizable"), Boolean::New(isolate, resizable))
      .Check();
  opts->Set(ctx, S(isolate, "alwaysOnTop"),
            Boolean::New(isolate, PBool(isolate, ctx, p, "alwaysOnTop")))
      .Check();
  opts->Set(ctx, S(isolate, "show"), Boolean::New(isolate, true)).Check();
  if (transparent) {
    opts->Set(ctx, S(isolate, "backgroundColor"), S(isolate, "#00000000"))
        .Check();
  }
  Local<Object> webprefs = Object::New(isolate);
  std::string preload = PStr(isolate, ctx, p, "preload");
  if (preload.empty()) preload = g_cfg_preload;
  webprefs->Set(ctx, S(isolate, "preload"), S(isolate, preload.c_str()))
      .Check();
  webprefs->Set(ctx, S(isolate, "contextIsolation"),
                Boolean::New(isolate, false))
      .Check();
  webprefs->Set(ctx, S(isolate, "nodeIntegration"),
                Boolean::New(isolate, false))
      .Check();
  webprefs->Set(ctx, S(isolate, "sandbox"), Boolean::New(isolate, false))
      .Check();
  opts->Set(ctx, S(isolate, "webPreferences"), webprefs).Check();

  Local<Function> ctor = g_winctor.Get(isolate).As<Function>();
  Local<Value> argv[1] = {opts};
  Local<Object> win =
      ctor->NewInstance(ctx, 1, argv).FromMaybe(Local<Object>());
  if (win.IsEmpty()) return Local<Value>();

  uint32_t id = static_cast<uint32_t>(PNum(isolate, ctx, p, "id", 0));
  g_windows[id].Reset(isolate, win);
  int32_t wc_id = static_cast<int32_t>(
      PNum(isolate, ctx,
           win->Get(ctx, S(isolate, "webContents")).ToLocalChecked()
               .As<Object>(),
           "id", -1));
  g_by_wc[wc_id] = id;

  WireWindow(isolate, ctx, id, wc_id, win,
             win->Get(ctx, S(isolate, "webContents")).ToLocalChecked()
                 .As<Object>(),
             PBool(isolate, ctx, p, "enableFileDrop"),
             PBool(isolate, ctx, p, "frameless"));

  std::string url = PStr(isolate, ctx, p, "url");
  if (!url.empty()) {
    Local<Value> u = S(isolate, url.c_str());
    CallWin1(isolate, ctx, win, nullptr, "loadURL", u);

  }
  return CallWin(isolate, ctx, win, nullptr, "getBounds");
}

// ---- method dispatch ----
static Local<Value> DispatchMethod(Isolate* isolate, Local<Context> ctx,
                                   const std::string& m, Local<Object> p) {
  Local<Object> win;
  auto simple = [&](const char* obj_key, const char* method) {
    return CallWin(isolate, ctx, GetWin(isolate, ctx, p), obj_key, method);
  };
  auto simple1 = [&](const char* obj_key, const char* method, Local<Value> a) {
    return CallWin1(isolate, ctx, GetWin(isolate, ctx, p), obj_key, method, a);
  };

  if (m == "create") return CreateWindow(isolate, ctx, p);
  if (m == "close" || m == "destroy") return simple(nullptr, "destroy");
  if (m == "show") return simple(nullptr, "show");
  if (m == "hide") return simple(nullptr, "hide");
  if (m == "focus") {
    Local<Object> w = GetWin(isolate, ctx, p);
    Local<Value> min = CallWin(isolate, ctx, w, nullptr, "isMinimized");
    if (!min.IsEmpty() && min->IsTrue()) {
      CallWin(isolate, ctx, w, nullptr, "restore");
    }
    return CallWin(isolate, ctx, w, nullptr, "focus");
  }
  if (m == "setTitle")
    return simple1(nullptr, "setTitle",
                   S(isolate, PStr(isolate, ctx, p, "title").c_str()));
  if (m == "setPosition") {
    Local<Object> w = GetWin(isolate, ctx, p);
    Local<Value> argv[2] = {
        Number::New(isolate, PNum(isolate, ctx, p, "x", 0)),
        Number::New(isolate, PNum(isolate, ctx, p, "y", 0))};
    Local<Function> fn = w->Get(ctx, S(isolate, "setPosition"))
                             .ToLocalChecked()
                             .As<Function>();
    return fn->Call(ctx, w, 2, argv).FromMaybe(Local<Value>());
  }
  if (m == "setSize") {
    Local<Object> w = GetWin(isolate, ctx, p);
    Local<Value> argv[2] = {
        Number::New(isolate, PNum(isolate, ctx, p, "width", 0)),
        Number::New(isolate, PNum(isolate, ctx, p, "height", 0))};
    Local<Function> fn =
        w->Get(ctx, S(isolate, "setSize")).ToLocalChecked().As<Function>();
    return fn->Call(ctx, w, 2, argv).FromMaybe(Local<Value>());
  }
  if (m == "setBounds") {
    Local<Object> w = GetWin(isolate, ctx, p);
    Local<Object> b = Object::New(isolate);
    const char* keys[4] = {"x", "y", "width", "height"};
    for (const char* k : keys) {
      Local<Value> v;
      if (p->Get(ctx, S(isolate, k)).ToLocal(&v) && !v->IsUndefined()) {
        b->Set(ctx, S(isolate, k), v).Check();
      }
    }
    Local<Value> argv[1] = {b};
    Local<Function> fn = w->Get(ctx, S(isolate, "setBounds"))
                             .ToLocalChecked()
                             .As<Function>();
    return fn->Call(ctx, w, 1, argv).FromMaybe(Local<Value>());
  }
  if (m == "getBounds") return simple(nullptr, "getBounds");
  if (m == "center") return simple(nullptr, "center");
  if (m == "setAlwaysOnTop")
    return simple1(nullptr, "setAlwaysOnTop",
                   Boolean::New(isolate, PBool(isolate, ctx, p, "v")));
  if (m == "setResizable")
    return simple1(nullptr, "setResizable",
                   Boolean::New(isolate, PBool(isolate, ctx, p, "v")));
  if (m == "setFullScreen")
    return simple1(nullptr, "setFullScreen",
                   Boolean::New(isolate, PBool(isolate, ctx, p, "v")));
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
                   S(isolate, PStr(isolate, ctx, p, "js").c_str()));
  if (m == "loadURL")
    return simple1(nullptr, "loadURL",
                   S(isolate, PStr(isolate, ctx, p, "url").c_str()));
  if (m == "reload") return simple("webContents", "reload");
  if (m == "forceReload") return simple("webContents", "reloadIgnoringCache");
  if (m == "openDevTools") {
    Local<Object> w = GetWin(isolate, ctx, p);
    Local<Object> wc = w->Get(ctx, S(isolate, "webContents"))
                           .ToLocalChecked()
                           .As<Object>();
    Local<Object> mode = Object::New(isolate);
    mode->Set(ctx, S(isolate, "mode"), S(isolate, "detach")).Check();
    Local<Value> argv[1] = {mode};
    Local<Function> fn = wc->Get(ctx, S(isolate, "openDevTools"))
                             .ToLocalChecked()
                             .As<Function>();
    return fn->Call(ctx, wc, 1, argv).FromMaybe(Local<Value>());
  }
  if (m == "setZoom")
    return simple1("webContents", "setZoomFactor",
                   Number::New(isolate, PNum(isolate, ctx, p, "v", 1)));
  if (m == "getZoom") return simple("webContents", "getZoomFactor");
  if (m == "setBackgroundColour")
    return simple1(nullptr, "setBackgroundColor",
                   S(isolate, PStr(isolate, ctx, p, "colour").c_str()));
  if (m == "setIgnoreMouseEvents")
    return simple1(nullptr, "setIgnoreMouseEvents",
                   Boolean::New(isolate, PBool(isolate, ctx, p, "v")));
  if (m == "copy") return simple("webContents", "copy");
  if (m == "paste") return simple("webContents", "paste");
  if (m == "cut") return simple("webContents", "cut");
  if (m == "undo") return simple("webContents", "undo");
  if (m == "redo") return simple("webContents", "redo");
  if (m == "selectAll") return simple("webContents", "selectAll");
  if (m == "delete") return simple("webContents", "delete");
  if (m == "setMinimumSize" || m == "setMaximumSize") {
    Local<Object> w = GetWin(isolate, ctx, p);
    Local<Value> argv[2] = {
        Number::New(isolate, PNum(isolate, ctx, p, "width", 0)),
        Number::New(isolate, PNum(isolate, ctx, p, "height", 0))};
    const char* fn = m == "setMinimumSize" ? "setMinimumSize" : "setMaximumSize";
    Local<Function> f = w->Get(ctx, S(isolate, fn)).ToLocalChecked().As<Function>();
    return f->Call(ctx, w, 2, argv).FromMaybe(Local<Value>());
  }
  if (m == "setParent") {
    Local<Object> w = GetWin(isolate, ctx, p);
    uint32_t parent_id = static_cast<uint32_t>(PNum(isolate, ctx, p, "parent", 0));
    Local<Object> parent;
    {
      auto it = g_windows.find(parent_id);
      if (it != g_windows.end()) parent = it->second.Get(isolate);
    }
    if (parent.IsEmpty()) {
      isolate->ThrowError("setParent: unknown parent window");
      return Local<Value>();
    }
    Local<Function> f = w->Get(ctx, S(isolate, "setParentWindow")).ToLocalChecked().As<Function>();
    Local<Value> argv[1] = {parent};
    return f->Call(ctx, w, 1, argv).FromMaybe(Local<Value>());
  }
  if (m == "startDrag" || m == "startResize") {
    // Experimental (WAILS_COMPAT_X11_DRAG=1): the self-managed pointer
    // tracking moves the Chromium window from outside its process, which
    // Chromium sometimes answers by unmapping it — under investigation.
    // Without the flag the host reports an explicit error instead.
    static const bool x11_drag = getenv("WAILS_COMPAT_X11_DRAG") != nullptr;
    if (!x11_drag) {
      isolate->ThrowError("frameless drag/resize is not available on the electron backend yet");
      return Local<Value>();
    }
    Local<Object> w = GetWin(isolate, ctx, p);
    // the native XID rides in the first 4 bytes of getNativeWindowHandle()
    Local<Function> gnh =
        w->Get(ctx, S(isolate, "getNativeWindowHandle")).ToLocalChecked().As<Function>();
    Local<Value> handle = gnh->Call(ctx, w, 0, nullptr).FromMaybe(Local<Value>());
    fprintf(stderr, "[go-bridge] startDrag/Resize dispatch entered\n");
    if (handle.IsEmpty() || !handle->IsArrayBufferView()) {
      isolate->ThrowError("startDrag: no native window handle");
      return Local<Value>();
    }
    auto view = handle.As<ArrayBufferView>();
    std::shared_ptr<BackingStore> bs = view->Buffer()->GetBackingStore();
    if (view->ByteLength() < 4) {
      isolate->ThrowError("startDrag: short native handle");
      return Local<Value>();
    }
    unsigned long xid = *reinterpret_cast<const uint32_t*>(
        static_cast<const char*>(bs->Data()) + view->ByteOffset());
    int direction = 8;  // move
    if (m == "startResize") {
      direction = x11_direction(PStr(isolate, ctx, p, "edge").c_str());
    }
    x11_move_resize(xid, direction);
    return Undefined(isolate);
  }
  if (m == "quit") {
    Local<Function> fn = g_app.Get(isolate)
                             ->Get(ctx, S(isolate, "quit"))
                             .ToLocalChecked()
                             .As<Function>();
    return fn->Call(ctx, g_app.Get(isolate), 0, nullptr)
        .FromMaybe(Local<Value>());
  }
  isolate->ThrowException(Exception::Error(S(isolate, ("unknown method " + m).c_str())));
  return Local<Value>();
}

// ---- stdin control reader (raw blocking thread — no libuv stream, so
// window destruction can never stall the poll; EOF = host gone) ----
static void LinesAsyncCb(uv_async_t*) {
  Isolate* isolate = g_main_isolate;
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  std::vector<std::string> batch;
  bool eof = false;
  {
    std::lock_guard<std::mutex> lk(g_lines_mu);
    batch.swap(g_lines);
    eof = g_lines_eof;
    g_lines_eof = false;
  }
  for (auto& line : batch) {
    if (line == "\x01" "EOF") {
      if (g_m_debug) ELog("control read ended (eof)");
      Local<Function> fn = g_app.Get(isolate)
                               ->Get(ctx, S(isolate, "quit"))
                               .ToLocalChecked()
                               .As<Function>();
      fn->Call(ctx, g_app.Get(isolate), 0, nullptr).FromMaybe(Local<Value>());
      return;
    }
    if (line.empty()) continue;
    TryCatch tc(isolate);
    Local<Value> parsed =
        JsonParse(isolate, ctx, S(isolate, line.c_str()));
    if (parsed.IsEmpty() || !parsed->IsObject()) {
      tc.Reset();
      continue;
    }
    Local<Object> msg = parsed.As<Object>();
    Local<Value> tv;
    if (msg->Get(ctx, S(isolate, "t")).ToLocal(&tv) && tv->IsString()) {
      String::Utf8Value tu(isolate, tv);
      if (strcmp(*tu, "resp") == 0) continue;  // no Electron->Go reqs pending
    }
    std::string method;
    {
      Local<Value> mv;
      if (!msg->Get(ctx, S(isolate, "m")).ToLocal(&mv) || !mv->IsString())
        continue;
      String::Utf8Value mu(isolate, mv);
      method.assign(*mu, mu.length());
    }
    int64_t id = static_cast<int64_t>(
        PNum(isolate, ctx, msg, "id", -1));
    Local<Value> pv;
    Local<Object> params = Object::New(isolate);
    if (msg->Get(ctx, S(isolate, "p")).ToLocal(&pv) && pv->IsObject()) {
      params = pv.As<Object>();
    }
    if (g_m_debug) ELog("dispatch %s id=%lld", method.c_str(),
                        (long long)id);
    Local<Value> result = DispatchMethod(isolate, ctx, method, params);
    if (tc.HasCaught()) {
      Local<Value> exc = tc.Exception();
      Local<String> s;
      std::string err = "Error";
      if (!exc.IsEmpty() && exc->ToString(ctx).ToLocal(&s)) {
        String::Utf8Value u(isolate, s);
        err.assign(*u, u.length());
      }
      SendRespErr(isolate, ctx, id, err.c_str());
      continue;
    }
    RespondValue(isolate, ctx, id, result);
  }
}

static void QuitAsyncCb(uv_async_t*) {
  Isolate* isolate = g_main_isolate;
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  Local<Function> fn = g_app.Get(isolate)
                           ->Get(ctx, S(isolate, "quit"))
                           .ToLocalChecked()
                           .As<Function>();
  fn->Call(ctx, g_app.Get(isolate), 0, nullptr).FromMaybe(Local<Value>());
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

// orphan protection layer 2: quit when re-parented (host SIGKILL'd)
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

static void PreloadErrorCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  std::string path, err;
  if (args.Length() >= 2 && args[1]->IsString()) {
    String::Utf8Value u(isolate, args[1]);
    path.assign(*u, u.length());
  }
  if (args.Length() >= 3) {
    Local<String> s;
    if (args[2]->ToString(ctx).ToLocal(&s)) {
      String::Utf8Value u(isolate, s);
      err.assign(*u, u.length());
    }
  }
  fprintf(stderr, "[wails-electron] preload-error %s: %s\n", path.c_str(),
          err.c_str());
}

static void NoopCb(const FunctionCallbackInfo<Value>& args) {}

static void WailsMessageCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  // event.sender.id -> windowID (numeric keys: the churning fix)
  uint32_t id = 0;
  if (args.Length() >= 1 && args[0]->IsObject()) {
    Local<Value> sender;
    if (args[0].As<Object>()->Get(ctx, S(isolate, "sender")).ToLocal(&sender) &&
        sender->IsObject()) {
      int32_t wc = static_cast<int32_t>(
          PNum(isolate, ctx, sender.As<Object>(), "id", -1));
      auto it = g_by_wc.find(wc);
      if (it != g_by_wc.end()) id = it->second;
    }
  }
  Local<Value> payload =
      args.Length() >= 2 ? Local<Value>(args[1]) : Undefined(isolate);
  if (g_m_debug && payload->IsString()) {
    String::Utf8Value pu(isolate, payload);
    fprintf(stderr, "[wails-electron] postMessage: %.60s\n", *pu);
  }
  // compat diagnostics passthrough (same shape as main.js logged)
  if (payload->IsString()) {
    TryCatch tc(isolate);
    Local<Value> parsed =
        JsonParse(isolate, ctx, payload.As<String>());
    if (!parsed.IsEmpty() && parsed->IsObject()) {
      Local<Value> name;
      if (parsed.As<Object>()
              ->Get(ctx, S(isolate, "name"))
              .ToLocal(&name) &&
          name->IsString()) {
        String::Utf8Value nu(isolate, name);
        if (strncmp(*nu, "compat:", 7) == 0) {
          Local<Value> data;
          std::string ds;
          if (parsed.As<Object>()
                  ->Get(ctx, S(isolate, "data"))
                  .ToLocal(&data)) {
            Local<String> s;
            if (data->ToString(ctx).ToLocal(&s)) {
              String::Utf8Value du(isolate, s);
              ds.assign(*du, du.length());
            }
          }
          fprintf(stderr, "[wails-electron] %s %s\n", *nu, ds.c_str());
        }
      }
    }
  }
  Local<Object> o = Object::New(isolate);
  o->Set(ctx, S(isolate, "t"), S(isolate, "ev")).Check();
  o->Set(ctx, S(isolate, "e"), S(isolate, "message")).Check();
  Local<Object> p = Object::New(isolate);
  p->Set(ctx, S(isolate, "id"), Integer::NewFromUnsigned(isolate, id)).Check();
  p->Set(ctx, S(isolate, "payload"), payload).Check();
  o->Set(ctx, S(isolate, "p"), p).Check();
  SendObj(isolate, ctx, o);
}

static void CompatPingCb(const FunctionCallbackInfo<Value>& args) {
  if (args.Length() >= 2) {
    args.GetReturnValue().Set(args[1]);
  }
}

static void ReadyCb(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  static pthread_t reader, guard;
  pthread_create(&reader, nullptr, ReaderThread, nullptr);
  pthread_create(&guard, nullptr, PpidGuardThread, nullptr);
  WinEvent(isolate, ctx, 0, "ready");
}

static void QuitAsyncFromSignal(const FunctionCallbackInfo<Value>& args);

// mainEntry(electron, cfg): the entire former main.js bootstrap.
static void MainEntry(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = I(args);
  HandleScope hs(isolate);
  Local<Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsObject()) {
    isolate->ThrowError("mainEntry(electron, cfg) requires the module");
    return;
  }
  Local<Object> electron = args[0].As<Object>();
  g_app.Reset(isolate,
              electron->Get(ctx, S(isolate, "app")).ToLocalChecked()
                  .As<Object>());
  g_winctor.Reset(isolate,
                  electron->Get(ctx, S(isolate, "BrowserWindow"))
                      .ToLocalChecked()
                      .As<Function>());
  g_ipcmain.Reset(isolate,
                  electron->Get(ctx, S(isolate, "ipcMain")).ToLocalChecked()
                      .As<Object>());
  g_m_debug = getenv("WAILS_ELECTRON_DEBUG") != nullptr &&
              strcmp(getenv("WAILS_ELECTRON_DEBUG"), "1") == 0;
  g_orig_ppid = static_cast<uint32_t>(getppid());
  g_main_isolate = isolate;

  if (args.Length() >= 2 && args[1]->IsObject()) {
    Local<Object> cfg = args[1].As<Object>();
    g_cfg_preload = PStr(isolate, ctx, cfg, "preload");
    g_cfg_bridge_path = PStr(isolate, ctx, cfg, "bridgePath");
    g_cfg_bridge_token = PStr(isolate, ctx, cfg, "bridgeToken");
    g_cfg_native_addon = PStr(isolate, ctx, cfg, "nativeAddon");
    if (!g_cfg_bridge_path.empty())
      setenv("WAILS_ELECTRON_BRIDGE_PATH", g_cfg_bridge_path.c_str(), 1);
    if (!g_cfg_native_addon.empty())
      setenv("WAILS_ELECTRON_NATIVE_ADDON", g_cfg_native_addon.c_str(), 1);
  }

  // Chromium switches: must land before app ready.
  {
    Local<Object> cl = g_app.Get(isolate)
                           ->Get(ctx, S(isolate, "commandLine"))
                           .ToLocalChecked()
                           .As<Object>();
    Local<Function> append =
        cl->Get(ctx, S(isolate, "appendSwitch")).ToLocalChecked()
            .As<Function>();
    Local<Value> argv[2] = {S(isolate, "disable-features"),
                            S(isolate, "CalculateNativeWinOcclusion")};
    append->Call(ctx, cl, 2, argv).FromMaybe(Local<Value>());
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
          Local<Value> a1[1] = {S(isolate, tok.c_str())};
          append->Call(ctx, cl, 1, a1).FromMaybe(Local<Value>());
        }
      }
    }
  }
  const char* nogpu = getenv("WAILS_ELECTRON_DISABLE_GPU");
  if (nogpu != nullptr && strcmp(nogpu, "1") == 0) {
    Local<Function> fn = g_app.Get(isolate)
                             ->Get(ctx, S(isolate, "disableHardwareAcceleration"))
                             .ToLocalChecked()
                             .As<Function>();
    fn->Call(ctx, g_app.Get(isolate), 0, nullptr).FromMaybe(Local<Value>());
  }

  // app event wiring
  auto on = [&](const char* ev, FunctionCallback cb) {
    Local<Function> fn =
        FunctionTemplate::New(isolate, cb)->GetFunction(ctx).ToLocalChecked();
    Local<Value> argv[2] = {S(isolate, ev), fn};
    Local<Value> ignored;
    g_app.Get(isolate)
        ->Get(ctx, S(isolate, "on"))
        .ToLocalChecked()
        .As<Function>()
        ->Call(ctx, g_app.Get(isolate), 2, argv)
        .ToLocal(&ignored);
  };
  on("preload-error", PreloadErrorCb);
  on("window-all-closed", NoopCb);  // the host owns the lifecycle

  // ipcMain handlers
  {
    Local<Object> ipc = g_ipcmain.Get(isolate);
    Local<Value> ignored;
    {
      Local<Function> fn = FunctionTemplate::New(isolate, WailsMessageCb)
                               ->GetFunction(ctx).ToLocalChecked();
      Local<Value> argv[2] = {S(isolate, "wails:message"), fn};
      ipc->Get(ctx, S(isolate, "on"))
          .ToLocalChecked()
          .As<Function>()
          ->Call(ctx, ipc, 2, argv)
          .ToLocal(&ignored);
    }
    {
      Local<Function> fn = FunctionTemplate::New(isolate, CompatPingCb)
                               ->GetFunction(ctx).ToLocalChecked();
      Local<Value> argv[2] = {S(isolate, "handle"), fn};
      // ipcMain.handle('compat:ping', fn)
      Local<Value> hargv[2] = {S(isolate, "compat:ping"), fn};
      ipc->Get(ctx, S(isolate, "handle"))
          .ToLocalChecked()
          .As<Function>()
          ->Call(ctx, ipc, 2, hargv)
          .ToLocal(&ignored);
    }
  }

  // signals
  {
    Local<Object> proc = ctx->Global()
                             ->Get(ctx, S(isolate, "process"))
                             .ToLocalChecked()
                             .As<Object>();
    Local<Function> onfn = proc->Get(ctx, S(isolate, "on"))
                               .ToLocalChecked()
                               .As<Function>();
    for (const char* sig : {"SIGTERM", "SIGINT"}) {
      Local<Function> fn = FunctionTemplate::New(isolate, QuitAsyncFromSignal)
                               ->GetFunction(ctx).ToLocalChecked();
      Local<Value> argv[2] = {S(isolate, sig), fn};
      Local<Value> ignored;
      onfn->Call(ctx, proc, 2, argv).ToLocal(&ignored);
    }
  }

  // async pumps: line dispatch + quit, on this loop
  g_loop = node::GetCurrentEventLoop(isolate);
  uv_async_init(g_loop, &g_lines_async, LinesAsyncCb);
  uv_async_init(g_loop, &g_quit_async, QuitAsyncCb);

  // app.whenReady().then(ReadyCb)
  {
    Local<Function> whenReady =
        g_app.Get(isolate)
            ->Get(ctx, S(isolate, "whenReady"))
            .ToLocalChecked()
            .As<Function>();
    Local<Value> prom;
    if (whenReady->Call(ctx, g_app.Get(isolate), 0, nullptr).ToLocal(&prom) &&
        prom->IsPromise()) {
      Local<Function> ok = FunctionTemplate::New(isolate, ReadyCb)
                               ->GetFunction(ctx).ToLocalChecked();
      prom.As<Promise>()->Then(ctx, ok).FromMaybe(Local<Promise>());
    }
  }
}


static void QuitAsyncFromSignal(const FunctionCallbackInfo<Value>& args) {
  uv_async_send(&g_quit_async);
}

// NODE_MODULE_INIT = context-aware registration (required for renderer
// loading in Electron; the N-API addon was context-aware automatically).
NODE_MODULE_INIT(/* exports, module, context */) {
  NODE_SET_METHOD(exports, "preloadInit", PreloadInit);
  NODE_SET_METHOD(exports, "mainEntry", MainEntry);
  NODE_SET_METHOD(exports, "close", Close);
}
