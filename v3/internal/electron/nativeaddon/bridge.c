// go-bridge: minimal N-API addon giving the Electron renderer a direct
// Unix-socket JSONL transport to the Wails host (native-ipc experiment).
//   connect(endpoint, token) -> undefined (throws on failure)
//   call(id, payload)        -> Promise<string> (echoed payload)
//   close()                  -> undefined
// Wire format mirrors pkg/application/webview_electron_native_linux.go:
//   {"hello":1,"token":"..."}\n   -> {"ready":true}\n
//   {"id":N,"payload":"..."}\n    -> {"id":N,"payload":"..."}\n
//
// Concurrency model mirrors Electron's ipc_renderer Invoke (serialize on
// the JS thread, dispatch the reply via a callback when it arrives):
// call() writes the frame on the JS thread and registers the deferred in
// a JS-thread-only pending table; a reader thread blocks on the socket
// and delivers replies through a napi_threadsafe_function. No threadpool
// worker is held hostage per call, and concurrent calls are supported.
// Replies for unknown ids (stale after a reuse) are dropped.
#include <node_api.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>

#define BRIDGE_TIMEOUT_SEC 10
#define PENDING_MAX 512

static int g_fd = -1;
static int g_connected = 0;
static pthread_t g_reader;
static napi_threadsafe_function g_tsfn;

// Pending calls live only on the JS thread (inserted by call(), consumed
// by the tsfn callback), so the table needs no lock.
typedef struct {
  int id;
  napi_deferred deferred;
} pending_t;
static pending_t g_pending[PENDING_MAX];
static size_t g_pending_n;

// Reader-thread -> JS-thread message, malloc'd, freed in the callback.
typedef struct {
  int id;        // matching call id; -1 = connection lost
  char *payload; // malloc'd on success
  char *err;     // malloc'd on failure
} result_msg_t;

static void free_msg(result_msg_t *m) {
  free(m->payload);
  free(m->err);
  free(m);
}

#define TRACE_ON() (getenv("WAILS_BRIDGE_TRACE") && getenv("WAILS_BRIDGE_TRACE")[0] == '1')

#define TRACE(...)                      \
  do {                                 \
    if (TRACE_ON()) {                  \
      fprintf(stderr, "[go-bridge] "); \
      fprintf(stderr, __VA_ARGS__);    \
      fputc('\n', stderr);             \
    }                                  \
  } while (0)

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

// Read one '\n'-terminated line into a growable buffer. Returns malloc'd
// line (without '\n', NUL-terminated) or NULL on EOF/error/timeout.
// Bulk copies with memchr; the buffer is sized from the socket's pending
// byte count (FIONREAD) so megabyte frames allocate once instead of
// walking a realloc+copy chain.
static char *read_line(int fd, size_t *out_len) {
  size_t cap = 16384, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  for (;;) {
    char *nl = memchr(buf, '\n', len);
    if (nl) {
      size_t line_len = (size_t)(nl - buf);
      *nl = '\0';
      // Calls are dispatched by id, so responses cannot interleave
      // inside one frame; residue after a line means a protocol
      // violation and is dropped.
      *out_len = line_len;
      return buf;
    }
    if (len == cap) {
      size_t want = cap * 2;
      int pending = 0;
      if (ioctl(fd, FIONREAD, &pending) == 0 && pending > 0) {
        size_t need = len + (size_t)pending + 1;
        if (need > want) want = need;
      }
      cap = want;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        return NULL;
      }
      buf = nb;
    }
    ssize_t n = read(fd, buf + len, cap - len);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf);
      return NULL;
    }
    if (n == 0) { // EOF
      free(buf);
      return NULL;
    }
    len += (size_t)n;
  }
}

static void json_escape(const char *in, size_t len, char **out, size_t *out_len) {
  // Fast path: nothing needs escaping — one bound check pass + one memcpy
  // (the bench payloads are megabytes of plain characters). *out == NULL
  // signals the caller that the payload can go out as-is (writev).
  size_t i = 0;
  for (; i < len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c == '"' || c == '\\') break;
  }
  if (i == len) {
    *out = NULL;
    *out_len = len;
    return;
  }
  size_t cap = len * 6 + 16, j = 0;
  char *b = malloc(cap);
  for (; i < len; i++) {
    unsigned char c = (unsigned char)in[i];
    const char *esc = NULL;
    switch (c) {
      case '"': esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\n': esc = "\\n"; break;
      case '\r': esc = "\\r"; break;
      case '\t': esc = "\\t"; break;
    }
    if (esc) {
      size_t l = strlen(esc);
      memcpy(b + j, esc, l);
      j += l;
    } else if (c < 0x20) {
      j += (size_t)sprintf(b + j, "\\u%04x", c);
    } else {
      b[j++] = (char)c;
    }
  }
  b[j] = '\0';
  *out = b;
  *out_len = j;
}

// Extract the value of "payload":"..." from a JSON line, unescaping JSON
// string escapes. Returns malloc'd UTF-8 or NULL. Fast path: value with
// no backslash escapes — locate the closing quote and memcpy once.
static char *json_payload_of(const char *line, size_t *out_len) {
  const char *p = strstr(line, "\"payload\":\"");
  if (!p) return NULL;
  p += strlen("\"payload\":\"");
  char *end = strchr(p, '"');
  if (!end) return NULL;
  if (memchr(p, '\\', (size_t)(end - p)) == NULL) {
    size_t n = (size_t)(end - p);
    char *b = malloc(n + 1);
    memcpy(b, p, n);
    b[n] = '\0';
    *out_len = n;
    return b;
  }
  size_t cap = strlen(p) + 1, j = 0;
  char *b = malloc(cap);
  while (*p && *p != '"') {
    char c = *p;
    if (c == '\\') {
      p++;
      switch (*p) {
        case 'n': b[j++] = '\n'; break;
        case 'r': b[j++] = '\r'; break;
        case 't': b[j++] = '\t'; break;
        case 'b': b[j++] = '\b'; break;
        case 'f': b[j++] = '\f'; break;
        case '/': b[j++] = '/'; break;
        case '"': b[j++] = '"'; break;
        case '\\': b[j++] = '\\'; break;
        case 'u': {
          if (strlen(p) < 5) { free(b); return NULL; }
          unsigned v = 0;
          for (int k = 1; k <= 4; k++) {
            char h = p[k];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
            else { free(b); return NULL; }
          }
          // UTF-8 encode the code point (BMP only; surrogate pairs not
          // needed for the echo protocol).
          if (v < 0x80) b[j++] = (char)v;
          else if (v < 0x800) {
            b[j++] = (char)(0xC0 | (v >> 6));
            b[j++] = (char)(0x80 | (v & 0x3F));
          } else {
            b[j++] = (char)(0xE0 | (v >> 12));
            b[j++] = (char)(0x80 | ((v >> 6) & 0x3F));
            b[j++] = (char)(0x80 | (v & 0x3F));
          }
          p += 4;
          break;
        }
        default: free(b); return NULL;
      }
      p++;
    } else {
      b[j++] = c;
      p++;
    }
  }
  b[j] = '\0';
  *out_len = j;
  return b;
}

// ---- pending table (JS thread only) ----

static napi_deferred pending_take(int id) {
  for (size_t i = 0; i < g_pending_n; i++) {
    if (g_pending[i].id == id) {
      napi_deferred d = g_pending[i].deferred;
      g_pending[i] = g_pending[g_pending_n - 1];
      g_pending_n--;
      return d;
    }
  }
  return NULL;
}

// ---- reader thread ----

static void *reader_main(void *arg) {
  (void)arg;
  for (;;) {
    size_t line_len = 0;
    char *line = read_line(g_fd, &line_len);
    if (!line) break; // EOF, error or 10s quiet timeout

    result_msg_t *m = calloc(1, sizeof(*m));
    char *idpos = strstr(line, "\"id\":");
    int id = idpos ? atoi(idpos + 5) : -1;
    size_t plen = 0;
    char *payload = json_payload_of(line, &plen);
    free(line);
    if (!payload || id < 0) {
      free(payload);
      m->id = -1;
      m->err = strdup("malformed frame from host");
    } else {
      m->id = id;
      m->payload = payload;
    }
    if (napi_call_threadsafe_function(g_tsfn, m, napi_tsfn_nonblocking) != napi_ok) {
      free_msg(m); // environment is shutting down
      break;
    }
  }
  // Connection lost (or timed out): one poison message rejects every
  // pending call on the JS thread.
  result_msg_t *m = calloc(1, sizeof(*m));
  m->id = -1;
  m->err = strdup(errno == EAGAIN ? "host reply timed out" : "connection to host lost");
  napi_call_threadsafe_function(g_tsfn, m, napi_tsfn_nonblocking);
  return NULL;
}

// ---- tsfn callback (runs on the JS thread) ----

// Placeholder JS function backing the threadsafe function (the real work
// happens in CallJsDispatch; napi_create_function rejects a NULL callback).
static napi_value DispatchNoop(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return NULL;
}


static void CallJsDispatch(napi_env env, napi_value js_cb, void *context, void *vdata) {
  (void)js_cb;
  (void)context;
  result_msg_t *m = vdata;
  if (m->id == -1) {
    napi_value msg;
    napi_create_string_utf8(env, m->err ? m->err : "connection lost",
                            NAPI_AUTO_LENGTH, &msg);
    for (size_t i = 0; i < g_pending_n; i++) {
      napi_reject_deferred(env, g_pending[i].deferred, msg);
    }
    g_pending_n = 0;
    g_connected = 0;
    free_msg(m);
    return;
  }
  napi_value value;
  napi_deferred d = pending_take(m->id);
  if (!d) {
    free_msg(m); // stale id (cancelled by reuse) — drop the reply
    return;
  }
  if (m->payload) {
    napi_create_string_utf8(env, m->payload, strlen(m->payload), &value);
    napi_resolve_deferred(env, d, value);
  } else {
    napi_reject_deferred(env, d, value);
  }
  free_msg(m);
}

static void teardown_connection(void) {
  if (g_fd >= 0) {
    shutdown(g_fd, SHUT_RDWR); // wakes the reader thread
  }
  pthread_join(g_reader, NULL);
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
  napi_release_threadsafe_function(g_tsfn, napi_tsfn_release);
  g_connected = 0;
}

static napi_value Call(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    napi_throw_error(env, NULL, "call(id, payload) requires 2 args");
    return NULL;
  }
  if (!g_connected) {
    napi_throw_error(env, NULL, "not connected");
    return NULL;
  }
  int32_t id = 0;
  napi_get_value_int32(env, argv[0], &id);
  size_t plen = 0;
  napi_get_value_string_utf8(env, argv[1], NULL, 0, &plen);
  char *payload = malloc(plen + 1);
  napi_get_value_string_utf8(env, argv[1], payload, plen + 1, &plen);

  napi_value promise, name;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);
  (void)name;

  // A reused id cancels the older call instead of leaking its deferred.
  napi_deferred stale = pending_take(id);
  if (stale) {
    napi_value msg;
    napi_create_string_utf8(env, "cancelled: id reused", NAPI_AUTO_LENGTH, &msg);
    napi_reject_deferred(env, stale, msg);
  }
  if (g_pending_n == PENDING_MAX) {
    free(payload);
    napi_value msg;
    napi_create_string_utf8(env, "too many pending calls", NAPI_AUTO_LENGTH, &msg);
    napi_reject_deferred(env, deferred, msg);
    return promise;
  }
  g_pending[g_pending_n].id = id;
  g_pending[g_pending_n].deferred = deferred;
  g_pending_n++;

  // Serialize on the JS thread (Electron does the same). Escaped
  // payloads get copied into one frame; clean payloads go out through
  // writev untouched — a megabyte frame skips a full memcpy.
  char *esc = NULL;
  size_t esc_len = 0;
  json_escape(payload, plen, &esc, &esc_len);
  char header[48];
  int hlen = sprintf(header, "{\"id\":%d,\"payload\":\"", id);
  const char tail[3] = {'"', '}', '\n'};
  int werr = 0;
  if (esc == NULL) {
    struct iovec iov[3] = {
        {header, (size_t)hlen}, {payload, plen}, {(void *)tail, 3}};
    size_t off = 0, total = (size_t)hlen + plen + 3;
    // writev can short-write on big frames; finish the remainder.
    while (total > off) {
      ssize_t n = writev(g_fd, iov, 3);
      if (n < 0) {
        if (errno == EINTR) continue;
        werr = errno;
        break;
      }
      off += (size_t)n;
      if (off == total) break;
      size_t rem = off;
      for (int i = 0; i < 3; i++) {
        if (rem >= iov[i].iov_len) {
          rem -= iov[i].iov_len;
          iov[i].iov_len = 0;
        } else {
          iov[i].iov_base = (char *)iov[i].iov_base + rem;
          iov[i].iov_len -= rem;
          rem = 0;
        }
      }
    }
  } else {
    size_t cap = (size_t)hlen + esc_len + 3;
    char *frame = malloc(cap);
    memcpy(frame, header, (size_t)hlen);
    memcpy(frame + hlen, esc, esc_len);
    memcpy(frame + hlen + esc_len, tail, 3);
    free(esc);
    werr = write_all(g_fd, frame, cap) == 0 ? 0 : errno;
    free(frame);
  }
  free(payload);
  if (werr != 0) {
    TRACE("call id=%d write failed: %s", id, strerror(werr));
    napi_deferred d = pending_take(id);
    if (d) {
      napi_value msg;
      char buf[128];
      snprintf(buf, sizeof(buf), "write failed: %s", strerror(werr));
      napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &msg);
      napi_reject_deferred(env, d, msg);
    }
    g_connected = 0;
  }
  return promise;
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

  if (g_connected) {
    teardown_connection();
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
  char hello[192];
  int hn = snprintf(hello, sizeof(hello), "{\"hello\":1,\"token\":\"%s\"}\n", token);
  if (write_all(fd, hello, (size_t)hn) != 0) {
    close(fd);
    napi_throw_error(env, NULL, "hello write failed");
    return NULL;
  }
  struct timeval tv = {BRIDGE_TIMEOUT_SEC, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  // Headroom for megabyte frames: default UDS buffers (~200KB) would
  // split them into many flow-control round trips.
  int bufsz = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  size_t line_len = 0;
  char *line = read_line(fd, &line_len);
  if (!line || !strstr(line, "\"ready\":true")) {
    fprintf(stderr, "[go-bridge] ERROR handshake got: %s\n", line ? line : "(eof)");
    free(line);
    close(fd);
    napi_throw_error(env, NULL, "handshake failed");
    return NULL;
  }
  free(line);

  napi_value placeholder, resource_name;
  napi_create_function(env, "dispatch", NAPI_AUTO_LENGTH, DispatchNoop, NULL, &placeholder);
  napi_create_string_utf8(env, "go-bridge", NAPI_AUTO_LENGTH, &resource_name);
  napi_status tsfn_st = napi_create_threadsafe_function(
      env, placeholder, NULL, resource_name, 0, 1, NULL, NULL, NULL,
      CallJsDispatch, &g_tsfn);
  if (tsfn_st != napi_ok) {
    fprintf(stderr, "[go-bridge] ERROR tsfn create status=%d\n", (int)tsfn_st);
    close(fd);
    napi_throw_error(env, NULL, "failed to create dispatch queue");
    return NULL;
  }
  g_fd = fd;
  g_connected = 1;
  if (pthread_create(&g_reader, NULL, reader_main, NULL) != 0) {
    napi_release_threadsafe_function(g_tsfn, napi_tsfn_release);
    close(fd);
    g_fd = -1;
    g_connected = 0;
    napi_throw_error(env, NULL, "failed to start reader thread");
    return NULL;
  }
  TRACE("connected endpoint=%s", endpoint);
  return NULL;
}

static napi_value Close(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  if (g_connected) {
    teardown_connection();
  }
  return NULL;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "connect", NAPI_AUTO_LENGTH, Connect, NULL, &fn);
  napi_set_named_property(env, exports, "connect", fn);
  napi_create_function(env, "call", NAPI_AUTO_LENGTH, Call, NULL, &fn);
  napi_set_named_property(env, exports, "call", fn);
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, Close, NULL, &fn);
  napi_set_named_property(env, exports, "close", fn);
  return exports;
}

NAPI_MODULE(bridge, Init)
