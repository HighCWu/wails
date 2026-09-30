// go-bridge: minimal N-API addon giving the Electron renderer a direct
// Unix-socket JSONL transport to the Wails host (native-ipc experiment).
//   connect(endpoint, token) -> undefined (throws on failure)
//   call(id, payload)        -> Promise<string> (echoed payload)
//   close()                  -> undefined
// Wire format mirrors pkg/application/webview_electron_native_linux.go:
//   {"hello":1,"token":"..."}\n   -> {"ready":true}\n
//   {"id":N,"payload":"..."}\n    -> {"id":N,"payload":"..."}\n
// Calls are serialized behind a mutex and executed as one blocking async
// work each: correct for sequential RPC; concurrent callers queue.
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
static pthread_mutex_t g_io_mu = PTHREAD_MUTEX_INITIALIZER;

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
// Bulk copies with memchr/memcpy — the payload sweep benches 1MB lines,
// so per-byte loops here would dominate the round trip.
static char *read_line(int fd, size_t *out_len) {
  size_t cap = 16384, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  for (;;) {
    char *nl = memchr(buf, '\n', len);
    if (nl) {
      size_t line_len = (size_t)(nl - buf);
      *nl = '\0';
      // Calls are strictly sequential (one outstanding frame), so a line
      // is always followed by exactly one newline at EOF of that frame —
      // trailing residue would mean a protocol violation; drop it.
      *out_len = line_len;
      return buf;
    }
    if (len == cap) {
      // Ask the socket how many bytes are pending and jump straight to
      // that size — avoids the realloc+copy growth chain on megabyte
      // frames (FIONREAD works on Unix sockets; fall back to doubling).
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
  // (the bench payloads are megabytes of plain characters).
  size_t i = 0;
  for (; i < len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c == '"' || c == '\\') break;
  }
  if (i == len) {
    char *b = malloc(len + 1);
    memcpy(b, in, len);
    b[len] = '\0';
    *out = b;
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

typedef struct {
  int id;
  char *payload; // owned copy
  size_t payload_len;
  // results
  char *result;
  size_t result_len;
  char errbuf[192];
  napi_deferred deferred;
  napi_async_work work;
} call_ctx_t;

static void call_execute(napi_env env, void *data) {
  (void)env;
  call_ctx_t *c = data;
  c->result = NULL;
  pthread_mutex_lock(&g_io_mu);
  if (g_fd < 0) {
    snprintf(c->errbuf, sizeof(c->errbuf), "not connected");
    pthread_mutex_unlock(&g_io_mu);
    return;
  }
  char *esc = NULL;
  size_t esc_len = 0;
  json_escape(c->payload, c->payload_len, &esc, &esc_len);
  // Manual frame assembly: snprintf("%s") would strlen-scan the payload
  // again (megabytes) on top of the copies we already do.
  size_t cap = esc_len + 64;
  char *frame = malloc(cap);
  int n = sprintf(frame, "{\"id\":%d,\"payload\":\"", c->id);
  memcpy(frame + n, esc, esc_len);
  memcpy(frame + n + esc_len, "\"}\n", 3);
  n += (int)esc_len + 3;
  free(esc);
  TRACE("call id=%d writing %d bytes", c->id, n);
  if (write_all(g_fd, frame, (size_t)n) != 0) {
    snprintf(c->errbuf, sizeof(c->errbuf), "write: %s", strerror(errno));
    free(frame);
    pthread_mutex_unlock(&g_io_mu);
    return;
  }
  free(frame);

  for (;;) {
    size_t line_len = 0;
    char *line = read_line(g_fd, &line_len);
    if (!line) {
      snprintf(c->errbuf, sizeof(c->errbuf), "read: %s",
               errno == EAGAIN ? "timeout" : strerror(errno));
      close(g_fd);
      g_fd = -1;
      pthread_mutex_unlock(&g_io_mu);
      return;
    }
    TRACE("call id=%d got line len=%zu", c->id, line_len);
    char *found = strstr(line, "\"payload\":\"");
    char *idpos = strstr(line, "\"id\":");
    int rid = idpos ? atoi(idpos + 5) : -1;
    if (found && rid == c->id) {
      c->result = json_payload_of(line, &c->result_len);
      free(line);
      if (!c->result) snprintf(c->errbuf, sizeof(c->errbuf), "bad response frame");
      pthread_mutex_unlock(&g_io_mu);
      return;
    }
    free(line); // not ours (e.g. late ready) — keep scanning
  }
}

static void call_complete(napi_env env, napi_status status, void *data) {
  call_ctx_t *c = data;
  if (status != napi_ok || c->errbuf[0]) {
    napi_value msg;
    napi_create_string_utf8(env, c->errbuf[0] ? c->errbuf : "async work failed",
                            NAPI_AUTO_LENGTH, &msg);
    napi_reject_deferred(env, c->deferred, msg);
  } else {
    napi_value res;
    napi_create_string_utf8(env, c->result, c->result_len, &res);
    napi_resolve_deferred(env, c->deferred, res);
  }
  free(c->result);
  free(c->payload);
  napi_delete_async_work(env, c->work);
  free(c);
}

static napi_value Call(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    napi_throw_error(env, NULL, "call(id, payload) requires 2 args");
    return NULL;
  }
  int32_t id = 0;
  napi_get_value_int32(env, argv[0], &id);
  size_t plen = 0;
  napi_get_value_string_utf8(env, argv[1], NULL, 0, &plen);
  call_ctx_t *c = calloc(1, sizeof(call_ctx_t));
  c->payload = malloc(plen + 1);
  c->id = id;
  napi_get_value_string_utf8(env, argv[1], c->payload, plen + 1, &c->payload_len);

  napi_value promise, name;
  napi_create_promise(env, &c->deferred, &promise);
  napi_create_string_utf8(env, "bridge-call", NAPI_AUTO_LENGTH, &name);
  napi_create_async_work(env, NULL, name, call_execute, call_complete, c,
                         &c->work);
  napi_queue_async_work(env, c->work);
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

  pthread_mutex_lock(&g_io_mu);
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    trace_err("socket");
    pthread_mutex_unlock(&g_io_mu);
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
    pthread_mutex_unlock(&g_io_mu);
    napi_throw_error(env, NULL, "connect failed");
    return NULL;
  }
  char hello[192];
  int hn = snprintf(hello, sizeof(hello), "{\"hello\":1,\"token\":\"%s\"}\n", token);
  if (write_all(fd, hello, (size_t)hn) != 0) {
    close(fd);
    pthread_mutex_unlock(&g_io_mu);
    napi_throw_error(env, NULL, "hello write failed");
    return NULL;
  }
  struct timeval tv = {BRIDGE_TIMEOUT_SEC, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  size_t line_len = 0;
  char *line = read_line(fd, &line_len);
  if (!line || !strstr(line, "\"ready\":true")) {
    fprintf(stderr, "[go-bridge] ERROR handshake got: %s\n", line ? line : "(eof)");
    free(line);
    close(fd);
    pthread_mutex_unlock(&g_io_mu);
    napi_throw_error(env, NULL, "handshake failed");
    return NULL;
  }
  free(line);
  g_fd = fd;
  pthread_mutex_unlock(&g_io_mu);
  TRACE("connected endpoint=%s", endpoint);
  return NULL;
}

static napi_value Close(napi_env env, napi_callback_info info) {
  (void)info;
  pthread_mutex_lock(&g_io_mu);
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
  pthread_mutex_unlock(&g_io_mu);
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
