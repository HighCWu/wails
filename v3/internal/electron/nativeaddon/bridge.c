// go-bridge: minimal N-API addon giving the Electron renderer a direct
// Unix-socket transport to the Wails host (native-ipc experiment).
//   setSerde(serializeFn, deserializeFn)  -> inject v8 module functions
//   connect(endpoint, token)              -> undefined
//   invoke(msgObj)                        -> Promise<responseObj>
//   close()                               -> undefined
//
// Frame v3: 4-byte LE length + raw bytes of ONE v8-serialized message.
// Every frame on the wire (HELLO, invoke, echo, response) is the same
// shape — a single v8.serialize() payload — so the JS side never touches
// framing/escaping: it passes plain JS objects, the addon calls the
// injected serialize/deserialize functions (V8 ValueSerializer, the same
// engine behind Electron's SerializeV8Value) and handles the wire.
//
// Concurrency model: all I/O runs synchronously on the JS thread behind
// one mutex — sequential RPC by design, mirroring Connect's handshake.
#include <node_api.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BRIDGE_TIMEOUT_SEC 10

static int g_fd = -1;
static int g_connected = 0;
static pthread_mutex_t g_io_mu = PTHREAD_MUTEX_INITIALIZER;
static napi_ref g_ser_fn = NULL;   // v8.serialize
static napi_ref g_deser_fn = NULL; // v8.deserialize

static void trace_err(const char *what) {
  fprintf(stderr, "[go-bridge] ERROR %s: %s\n", what, strerror(errno));
}

static int write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

// Per-connection read context: bytes after a consumed message are kept
// for the next read instead of being dropped. Lives in the caller's
// frame; reset via memset when a connection is torn down.
typedef struct {
  char *buf;
  size_t cap, len;
} read_ctx_t;

static void rctx_free(read_ctx_t *c) {
  free(c->buf);
  memset(c, 0, sizeof(*c));
}

// Read exactly n bytes (length-prefix bodies may contain NULs/newlines).
static int read_exact(read_ctx_t *ctx, int fd, size_t n) {
  if (ctx->cap < n) {
    ctx->cap = n + 1024;
    char *nb = realloc(ctx->buf, ctx->cap);
    if (!nb) return -1;
    ctx->buf = nb;
  }
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, ctx->buf + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) {
      errno = ECONNRESET;
      return -1;
    }
    got += (size_t)r;
  }
  return 0;
}

// ---- serde bridge: C calls the injected v8 serialize/deserialize ----

// call a 1-arg JS function (held by napi_ref) synchronously; returns the
// result value, or NULL with a pending exception on JS throw.
static napi_value call_ref1(napi_env env, napi_ref fn_ref, napi_value arg) {
  napi_value fn = NULL, undef = NULL, res = NULL;
  napi_get_reference_value(env, fn_ref, &fn);
  napi_get_undefined(env, &undef);
  napi_call_function(env, undef, fn, 1, &arg, &res);
  return res;
}

// helper: get Buffer/TypedArray/ArrayBuffer bytes from a napi_value
static int value_bytes(napi_env env, napi_value v, char **out, size_t *out_len) {
  bool is_buf = false, is_ta = false, is_ab = false;
  napi_is_buffer(env, v, &is_buf);
  napi_is_typedarray(env, v, &is_ta);
  napi_is_arraybuffer(env, v, &is_ab);
  if (is_buf) {
    void *d = NULL;
    napi_get_buffer_info(env, v, &d, out_len);
    *out = d;
    return 0;
  }
  if (is_ta) {
    void *d = NULL;
    size_t alen = 0;
    napi_get_typedarray_info(env, v, NULL, &alen, &d, NULL, NULL);
    *out = d;
    *out_len = alen;
    return 0;
  }
  if (is_ab) {
    void *d = NULL;
    napi_get_arraybuffer_info(env, v, &d, out_len);
    *out = d;
    return 0;
  }
  return -1;
}

// SetSerde(serializeFn, deserializeFn): inject the v8 module functions.
// Held as napi_refs; the addon NEVER re-implements the format.
static napi_value SetSerde(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    napi_throw_error(env, NULL, "setSerde(serializeFn, deserializeFn) requires 2 args");
    return NULL;
  }
  if (g_ser_fn) napi_delete_reference(env, g_ser_fn);
  if (g_deser_fn) napi_delete_reference(env, g_deser_fn);
  napi_create_reference(env, argv[0], 1, &g_ser_fn);
  napi_create_reference(env, argv[1], 1, &g_deser_fn);
  return NULL;
}

static napi_value DispatchNoop(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return NULL;
}

static void teardown_connection(const char *who) {
  if (g_fd >= 0) {
    fprintf(stderr, "[go-bridge] teardown from=%s fd=%d connected=%d\n", who, g_fd, g_connected);
    shutdown(g_fd, SHUT_RDWR);
    close(g_fd);
    g_fd = -1;
  }
  g_connected = 0;
}

static napi_value Connect(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    napi_throw_error(env, NULL, "connect(endpoint, token) requires 2 args");
    return NULL;
  }
  char endpoint[256] = {0}, token[128] = {0};
  size_t n = 0;
  napi_get_value_string_utf8(env, argv[0], endpoint, sizeof(endpoint) - 1, &n);
  napi_get_value_string_utf8(env, argv[1], token, sizeof(token) - 1, &n);

  if (g_fd >= 0) {
    teardown_connection("connect-retry");
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    trace_err("socket");
    napi_throw_error(env, NULL, "socket failed");
    return NULL;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, endpoint, sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    trace_err("connect");
    close(fd);
    napi_throw_error(env, NULL, "connect failed");
    return NULL;
  }
  // HELLO as a v3 frame: length-prefixed JSON (the serde bridge is not
  // needed for this fixed, tiny message)
  char hello[192];
  int hn = snprintf(hello, sizeof(hello), "{\"hello\":1,\"token\":\"%s\"}\n", token);
  uint8_t hlen4[4] = {(uint8_t)(hn & 0xFF), (uint8_t)((hn >> 8) & 0xFF),
                      (uint8_t)((hn >> 16) & 0xFF), (uint8_t)((hn >> 24) & 0xFF)};
  if (write_all(fd, (char *)hlen4, 4) != 0 || write_all(fd, hello, (size_t)hn) != 0) {
    close(fd);
    napi_throw_error(env, NULL, "hello write failed");
    return NULL;
  }
  struct timeval tv = {BRIDGE_TIMEOUT_SEC, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int bufsz = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  // ready response: 4-byte LE length + JSON bytes
  uint8_t rlen4[4];
  size_t got = 0;
  while (got < 4) {
    ssize_t n = read(fd, rlen4 + got, 4 - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      trace_err("ready len read");
      close(fd);
      napi_throw_error(env, NULL, "ready read failed");
      return NULL;
    }
    if (n == 0) {
      fprintf(stderr, "[go-bridge] ERROR ready read: EOF\n");
      close(fd);
      napi_throw_error(env, NULL, "handshake failed");
      return NULL;
    }
    got += (size_t)n;
  }
  uint32_t rlen = (uint32_t)rlen4[0] | ((uint32_t)rlen4[1] << 8) |
                  ((uint32_t)rlen4[2] << 16) | ((uint32_t)rlen4[3] << 24);
  if (rlen == 0 || rlen > 1024 * 1024) {
    fprintf(stderr, "[go-bridge] ERROR ready len=%u\n", rlen);
    close(fd);
    napi_throw_error(env, NULL, "handshake failed");
    return NULL;
  }
  char *ready = malloc(rlen + 1);
  got = 0;
  while (got < rlen) {
    ssize_t n = read(fd, ready + got, rlen - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(ready);
      close(fd);
      napi_throw_error(env, NULL, "ready read failed");
      return NULL;
    }
    if (n == 0) {
      free(ready);
      close(fd);
      napi_throw_error(env, NULL, "ready read failed");
      return NULL;
    }
    got += (size_t)n;
  }
  ready[rlen] = '\0';
  int ok = strstr(ready, "\"ready\"") != NULL;
  if (!ok) fprintf(stderr, "[go-bridge] ERROR ready got: %.80s\n", ready);
  free(ready);
  if (!ok) {
    close(fd);
    napi_throw_error(env, NULL, "handshake failed");
    return NULL;
  }
  g_fd = fd;
  g_connected = 1;
  return NULL;
}

// invoke(msgObj) -> Promise<{status, contentType, body:Buffer}> for
// channel:"http"; channel:"echo" answers with the same message echoed.
// The msgObj is v8-serialized on the JS boundary (one napi_call into the
// injected v8.serialize — the SAME serializer Electron's IPC uses), so
// the wire carries a single v8-serialized message per length prefix.
static napi_value Invoke(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1) {
    napi_throw_error(env, NULL, "invoke(msgObj) requires 1 arg");
    return NULL;
  }
  if (!g_connected) {
    napi_throw_error(env, NULL, "not connected");
    return NULL;
  }
  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);

  pthread_mutex_lock(&g_io_mu);
  if (!g_connected) {
    pthread_mutex_unlock(&g_io_mu);
    napi_value mv;
    napi_create_string_utf8(env, "not connected", NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    return promise;
  }
  // 1. serialize: call the injected v8.serialize(msgObj) -> Buffer
  napi_value ser_fn = NULL, this_undef = NULL, ser_res = NULL;
  napi_get_reference_value(env, g_ser_fn, &ser_fn);
  napi_get_undefined(env, &this_undef);
  napi_call_function(env, this_undef, ser_fn, 1, argv, &ser_res);
  // 2. bytes out
  char *sbytes = NULL;
  size_t slen = 0;
  if (value_bytes(env, ser_res, &sbytes, &slen) != 0) {
    pthread_mutex_unlock(&g_io_mu);
    napi_value mv;
    napi_create_string_utf8(env, "serialize: not binary", NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    pthread_mutex_unlock(&g_io_mu);
    return promise;
  }
  // 3. wire: 4-byte LE length + bytes
  uint8_t len4[4] = {(uint8_t)(slen & 0xFF), (uint8_t)((slen >> 8) & 0xFF),
                     (uint8_t)((slen >> 16) & 0xFF), (uint8_t)((slen >> 24) & 0xFF)};
  int werr = write_all(g_fd, (char *)len4, 4) != 0 ? errno : 0;
  if (werr == 0) werr = write_all(g_fd, sbytes, slen) != 0 ? errno : 0;
  // 4. response: 4-byte LE length + v8-serialized reply
  uint8_t rlen4[4];
  size_t got = 0;
  while (got < 4 && werr == 0) {
    ssize_t n = read(g_fd, rlen4 + got, 4 - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      werr = errno;
      break;
    }
    if (n == 0) {
      werr = ECONNRESET;
      break;
    }
    got += (size_t)n;
  }
  if (werr != 0) {
    pthread_mutex_unlock(&g_io_mu);
    teardown_connection("invoke-len");
    napi_value mv;
    napi_create_string_utf8(env, "response read failed", NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    return promise;
  }
  uint32_t rlen = (uint32_t)rlen4[0] | ((uint32_t)rlen4[1] << 8) |
                  ((uint32_t)rlen4[2] << 16) | ((uint32_t)rlen4[3] << 24);
  if (rlen > 256u * 1024u * 1024u) {
    pthread_mutex_unlock(&g_io_mu);
    teardown_connection("invoke-toolarge");
    fprintf(stderr, "[go-bridge] ERROR response len=%u too large\n", rlen);
    napi_value mv;
    napi_create_string_utf8(env, "response too large", NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    return promise;
  }
  char *rbuf = malloc(rlen + 1);
  void *bytes_val_data = NULL;
  got = 0;
  while (got < rlen) {
    ssize_t n = read(g_fd, rbuf + got, rlen - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      werr = errno;
      break;
    }
    if (n == 0) {
      werr = ECONNRESET;
      break;
    }
    got += (size_t)n;
  }
  pthread_mutex_unlock(&g_io_mu);
  if (werr != 0) {
    free(rbuf);
    teardown_connection("invoke-body");
    char msg2[160];
    snprintf(msg2, sizeof(msg2), "response read failed: %s", strerror(werr));
    napi_value mv;
    napi_create_string_utf8(env, msg2, NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    return promise;
  }
  // 5. deserialize: call the injected v8.deserialize(bytes) -> JS object
  napi_value deser_fn = NULL, bytes_val = NULL, resp_obj = NULL;
  napi_get_reference_value(env, g_deser_fn, &deser_fn);
  napi_create_buffer(env, rlen, &bytes_val_data, &bytes_val);
  memcpy(bytes_val_data, rbuf, rlen);
  napi_get_undefined(env, &this_undef);
  napi_call_function(env, this_undef, deser_fn, 1, &bytes_val, &resp_obj);
  napi_resolve_deferred(env, deferred, resp_obj);
  free(rbuf);
  return promise;
}

static napi_value Close(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  if (g_fd >= 0) {
    teardown_connection("close-export");
  }
  return NULL;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "setSerde", NAPI_AUTO_LENGTH, SetSerde, NULL, &fn);
  napi_set_named_property(env, exports, "setSerde", fn);
  napi_create_function(env, "connect", NAPI_AUTO_LENGTH, Connect, NULL, &fn);
  napi_set_named_property(env, exports, "connect", fn);
  napi_create_function(env, "invoke", NAPI_AUTO_LENGTH, Invoke, NULL, &fn);
  napi_set_named_property(env, exports, "invoke", fn);
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, Close, NULL, &fn);
  napi_set_named_property(env, exports, "close", fn);
  return exports;
}

NAPI_MODULE(bridge, Init)
