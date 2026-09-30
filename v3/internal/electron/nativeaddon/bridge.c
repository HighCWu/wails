// go-bridge: minimal N-API addon giving the Electron renderer a direct
// Unix-socket transport to the Wails host (native-ipc experiment).
//   connect(endpoint, token)            -> undefined
//   call(id, payload)                   -> Promise<string>   (echo frame)
//   callBin(id, headerJson, bodyBytes)  -> Promise<{status, contentType, body:Buffer}>
//   close()                             -> undefined
// Wire format (pkg/application/webview_electron_native_linux.go):
//   v1 echo : {"id":N,"payload":"..."}\n            -> same, echoed
//   v2 http : {"id":N,..,"bodyLen":B,"headers":{..}}\n + <B raw bytes>
//             -> {"id":N,"status":S,"contentType":"..","bodyLen":B}\n + <B bytes>
//
// Concurrency model: all I/O runs synchronously on the JS thread behind
// one mutex — sequential RPC by design (the benchmark and the bindings
// call sites are sequential). Calls queue; responses are matched by id
// on the wire. This mirrors Connect's handshake, which has always run
// this way; the earlier tsfn/reader-thread variant deadlocked (join
// from the JS thread) and raced its read buffer across reloads.
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

static int g_fd = -1;
static int g_connected = 0;
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

// Per-connection read context: bytes after a newline (or a binary body)
// are kept for the next read instead of being dropped. Lives in the
// caller's frame; reset via memset when a connection is torn down.
typedef struct {
  char *buf;
  size_t cap, len;
} read_ctx_t;

static void rctx_free(read_ctx_t *c) {
  free(c->buf);
  memset(c, 0, sizeof(*c));
}

// Read one '\n'-terminated line. Returns a pointer into the ctx buffer
// (valid until the next read on this ctx) or NULL on EOF/error/timeout.
static char *read_line_ctx(read_ctx_t *ctx, int fd, size_t *out_len) {
  if (!ctx->buf) {
    ctx->cap = 16384;
    ctx->buf = malloc(ctx->cap);
    if (!ctx->buf) return NULL;
  }
  for (;;) {
    char *nl = memchr(ctx->buf, '\n', ctx->len);
    if (nl) {
      size_t line_len = (size_t)(nl - ctx->buf);
      *nl = '\0';
      // consume the line: move any residue (the next frame's start, or a
      // v2 body) to the front so the next read continues after it
      // Copy the line out: the residue (a v2 body, or the next frame)
      // moves to the front of the shared buffer and would otherwise
      // overwrite the caller's view of the line mid-parse.
      char *copy = malloc(line_len + 1);
      if (!copy) {
        ctx->len = 0;
        return NULL;
      }
      memcpy(copy, ctx->buf, line_len);
      copy[line_len] = '\0';
      size_t rest = ctx->len - (line_len + 1);
      if (rest > 0) memmove(ctx->buf, nl + 1, rest);
      ctx->len = rest;
      *out_len = line_len;
      return copy; // caller frees
    }
    if (ctx->len == ctx->cap) {
      size_t want = ctx->cap * 2;
      int pending = 0;
      if (ioctl(fd, FIONREAD, &pending) == 0 && pending > 0) {
        size_t need = ctx->len + (size_t)pending + 1;
        if (need > want) want = need;
      }
      ctx->cap = want;
      char *nb = realloc(ctx->buf, ctx->cap);
      if (!nb) {
        rctx_free(ctx);
        return NULL;
      }
      ctx->buf = nb;
    }
    ssize_t n = read(fd, ctx->buf + ctx->len, ctx->cap - ctx->len);
    if (n < 0) {
      if (errno == EINTR) continue;
      rctx_free(ctx);
      return NULL;
    }
    if (n == 0) {
      rctx_free(ctx);
      return NULL;
    }
    ctx->len += (size_t)n;
  }
}

// Read exactly n bytes (a binary body — may contain newlines/NULs).
static char *read_exact_ctx(read_ctx_t *ctx, int fd, size_t n) {
  char *out = malloc(n + 1);
  if (!out) return NULL;
  size_t have = ctx->len < n ? ctx->len : n;
  if (n >= 65536) {
    fprintf(stderr, "[bt] exact n=%zu residue=%zu\n", n, ctx->len);
    if (ctx->len > 0) {
      fprintf(stderr, "[bt] residue HEAD %.300s\n", ctx->buf);
      size_t tail_start = ctx->len > 200 ? ctx->len - 200 : 0;
      fprintf(stderr, "[bt] residue TAIL %.200s\n", ctx->buf + tail_start);
      // byte-level diff anchors: find where the current response header
      // would start ("{"bodyLen"") if present in the residue
      char *bp = strstr(ctx->buf, "\"bodyLen\":");
      fprintf(stderr, "[bt] residue contains bodyLen-hdr at offset: %td\n",
              bp ? (ptrdiff_t)(bp - ctx->buf) : -1);
    }
  }
  fprintf(stderr, "[bt] exact n=%zu residue=%zu\n", n, ctx->len);
  memcpy(out, ctx->buf, have);
  memmove(ctx->buf, ctx->buf + have, ctx->len - have);
  ctx->len -= have;
  size_t got = have;
  while (got < n) {
    ssize_t r = read(fd, out + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      free(out);
      return NULL;
    }
    if (r == 0) {
      errno = ECONNRESET;
      free(out);
      return NULL;
    }
    got += (size_t)r;
  }
  out[n] = '\0';
  return out;
}

static char *json_payload_of(const char *line, size_t *out_len);

// Extract the value of "payload":"..." from a JSON line, unescaping JSON
// string escapes. Returns malloc'd UTF-8 or NULL. Fast path: value with
// no backslash escapes — locate the closing quote and memcpy once.
static char *json_payload_of_impl(const char *line, size_t *out_len) {
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
            else { free(b); return NULL; }
          }
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

static char *json_payload_of(const char *line, size_t *out_len) {
  return json_payload_of_impl(line, out_len);
}

static napi_value DispatchNoop(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return NULL;
}

static void teardown_connection(void) {
  if (g_fd >= 0) {
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
  int bufsz = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  read_ctx_t hctx = {0};
  size_t line_len = 0;
  char *line = read_line_ctx(&hctx, fd, &line_len);
  if (!line || !strstr(line, "\"ready\":true")) {
    fprintf(stderr, "[go-bridge] ERROR handshake got: %s\n", line ? line : "(eof)");
    rctx_free(&hctx);
    close(fd);
    napi_throw_error(env, NULL, "handshake failed");
    return NULL;
  }
  rctx_free(&hctx);
  g_fd = fd;
  g_connected = 1;
  TRACE("connected endpoint=%s", endpoint);
  return NULL;
}

static napi_value Close(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  if (g_fd >= 0) {
    teardown_connection();
  }
  return NULL;
}

// ---- synchronous call implementations (JS thread, io mutex held) ----

// read_frame_response: one JSONL line response whose "payload" field is
// the echoed value. Returns a malloc'd copy or NULL.
static char *read_echo_response(read_ctx_t *ctx, int fd, int want_id, size_t *out_len) {
  for (;;) {
    size_t line_len = 0;
    char *line = read_line_ctx(ctx, fd, &line_len);
    if (!line) return NULL;
    char *idpos = strstr(line, "\"id\":");
    int id = idpos ? atoi(idpos + 5) : -1;
    if (id != want_id) {
      fprintf(stderr, "[bt] echo-skip id=%d line=%.40s residue=%zu\n", id, line, ctx->len);
      continue; // not ours (stale/foreign frame)
    }
    size_t plen = 0;
    char *payload = json_payload_of(line, &plen);
    if (!payload) return NULL;
    *out_len = plen;
    return payload;
  }
}

// read_http_response: two-part response (JSONL header + raw body).
static char *read_http_response(read_ctx_t *ctx, int fd, int want_id,
                                int *status, char *ctype, size_t ctype_sz,
                                size_t *body_len) {
  for (;;) {
    size_t line_len = 0;
    char *line = read_line_ctx(ctx, fd, &line_len);
    if (!line) return NULL;
    char *idpos = strstr(line, "\"id\":");
    int id = idpos ? atoi(idpos + 5) : -1;
    char *sp = strstr(line, "\"status\":");
    char *bp = strstr(line, "\"bodyLen\":");
    size_t blen = bp ? (size_t)atol(bp + 10) : 0;
    char *cp = strstr(line, "\"contentType\":\"");
    if (cp) {
      char *e = strchr(cp + 15, '"');
      size_t l = e ? (size_t)(e - (cp + 15)) : 0;
      if (l >= ctype_sz) l = ctype_sz - 1;
      memcpy(ctype, cp + 15, l);
      ctype[l] = '\0';
    }
    free(line); // shares the ctx buffer — extract before reading the body
    char *body = read_exact_ctx(ctx, fd, blen);
    if (!body) return NULL;
    *body_len = blen;
    return body;
  }
}

static void json_escape(const char *in, size_t len, char **out, size_t *out_len) {
  // Fast path: nothing needs escaping — *out == NULL signals the caller
  // that the payload can go out as-is (writev).
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
  // copy the verbatim prefix [0, i) first — everything before the first
  // escapable byte must survive (bug class: dropped leading '{')
  memcpy(b, in, i);
  j = i;
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

static void set_and_resolve_string(napi_env env, napi_deferred d, const char *s) {
  napi_value v;
  napi_create_string_utf8(env, s, strlen(s), &v);
  napi_resolve_deferred(env, d, v);
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
  char *payload = malloc(plen + 1);
  size_t actual = 0;
  napi_get_value_string_utf8(env, argv[1], payload, plen + 1, &actual);

  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);

  pthread_mutex_lock(&g_io_mu);
  if (!g_connected) {
    pthread_mutex_unlock(&g_io_mu);
    free(payload);
    napi_throw_error(env, NULL, "not connected");
    return NULL;
  }
  char *esc = NULL;
  size_t esc_len = 0;
  json_escape(payload, actual, &esc, &esc_len);
  char header[48];
  int hlen = sprintf(header, "{\"id\":%d,\"payload\":\"", id);
  int werr = 0;
  if (esc == NULL) {
    const char tail[3] = {'"', '}', '\n'};
    struct iovec iov[3] = {
        {header, (size_t)hlen}, {payload, actual}, {(void *)tail, 3}};
    size_t off = 0, total = (size_t)hlen + actual + 3;
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
    const char t[3] = {'"', '}', '\n'};
    memcpy(frame + hlen + esc_len, t, 3);
    free(esc);
    werr = write_all(g_fd, frame, cap) == 0 ? 0 : errno;
    free(frame);
  }
  free(payload);

  read_ctx_t ctx = {0};
  size_t rlen = 0;
  char *resp = werr ? NULL : read_echo_response(&ctx, g_fd, id, &rlen);
  rctx_free(&ctx);
  pthread_mutex_unlock(&g_io_mu);
  if (werr != 0 || !resp) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s", werr ? strerror(werr) : "host reply failed");
    napi_value mv;
    napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    if (werr != 0) g_connected = 0;
    return promise;
  }
  set_and_resolve_string(env, deferred, resp);
  free(resp);
  return promise;
}

static napi_value CallBin(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 3) {
    napi_throw_error(env, NULL, "callBin(id, header, body) requires 3 args");
    return NULL;
  }
  int32_t id = 0;
  napi_get_value_int32(env, argv[0], &id);
  size_t hlen = 0;
  napi_get_value_string_utf8(env, argv[1], NULL, 0, &hlen);
  char *header = malloc(hlen + 2);
  size_t hactual = 0;
  napi_get_value_string_utf8(env, argv[1], header, hlen + 1, &hactual);
  header[hactual] = '\n';
  hactual += 1;

  // body: Buffer / TypedArray / ArrayBuffer (raw bytes) or string (UTF-8)
  char *body = NULL;
  size_t blen = 0;
  bool is_buf = false, is_ta = false, is_ab = false;
  napi_is_buffer(env, argv[2], &is_buf);
  napi_is_typedarray(env, argv[2], &is_ta);
  napi_is_arraybuffer(env, argv[2], &is_ab);
  if (is_buf) {
    void *d = NULL;
    napi_get_buffer_info(env, argv[2], &d, &blen);
    body = malloc(blen + 1);
    memcpy(body, d, blen);
  } else if (is_ta) {
    void *d = NULL;
    size_t alen = 0;
    napi_get_typedarray_info(env, argv[2], NULL, &alen, &d, NULL, NULL);
    blen = alen;
    body = malloc(blen + 1);
    memcpy(body, d, blen);
  } else if (is_ab) {
    void *d = NULL;
    size_t alen = 0;
    napi_get_arraybuffer_info(env, argv[2], &d, &alen);
    blen = alen;
    body = malloc(blen + 1);
    memcpy(body, d, blen);
  } else {
    size_t slen = 0;
    napi_get_value_string_utf8(env, argv[2], NULL, 0, &slen);
    body = malloc(slen + 1);
    napi_get_value_string_utf8(env, argv[2], body, slen + 1, &slen);
    blen = slen;
  }

  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);

  pthread_mutex_lock(&g_io_mu);
  if (!g_connected) {
    pthread_mutex_unlock(&g_io_mu);
    free(header);
    free(body);
    napi_throw_error(env, NULL, "not connected");
    return NULL;
  }
  struct iovec iov[2] = {{header, hactual}, {body, blen}};
  size_t off = 0, total = hactual + blen;
  int werr = 0;
  while (total > off) {
    ssize_t n = writev(g_fd, iov, 2);
    if (n < 0) {
      if (errno == EINTR) continue;
      werr = errno;
      break;
    }
    off += (size_t)n;
    if (off == total) break;
    size_t rem = off;
    for (int i = 0; i < 2; i++) {
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
  free(header);
  free(body);

  read_ctx_t ctx = {0};
  int status = 200;
  char ctype[128] = "application/octet-stream";
  size_t rlen = 0;
  char *resp = werr ? NULL : read_http_response(&ctx, g_fd, id, &status, ctype, sizeof(ctype), &rlen);
  rctx_free(&ctx);
  pthread_mutex_unlock(&g_io_mu);
  if (werr != 0 || !resp) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s", werr ? strerror(werr) : "host reply failed");
    napi_value mv;
    napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    if (werr != 0) g_connected = 0;
    return promise;
  }
  napi_value obj, val;
  napi_create_object(env, &obj);
  napi_create_int32(env, status, &val);
  napi_set_named_property(env, obj, "status", val);
  napi_create_string_utf8(env, ctype, NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, obj, "contentType", val);
  void *copy = NULL;
  napi_create_buffer(env, rlen, &copy, &val);
  if (copy && rlen) memcpy(copy, resp, rlen);
  napi_set_named_property(env, obj, "body", val);
  napi_resolve_deferred(env, deferred, obj);
  free(resp);
  return promise;
}

// Invoke(msgBytes) -> Promise<responseBytes>
// Frame v3: 4-byte LE length + raw message bytes (a v8-serialized
// invoke object). Fully synchronous on the JS thread behind the io
// mutex — sequential RPC by design, mirroring the handshake.
static napi_value Invoke(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1) {
    napi_throw_error(env, NULL, "invoke(msg) requires 1 arg");
    return NULL;
  }
  if (!g_connected) {
    napi_throw_error(env, NULL, "not connected");
    return NULL;
  }
  // msg: Buffer / TypedArray / ArrayBuffer / string
  char *msg = NULL;
  size_t mlen = 0;
  bool is_buf = false, is_ta = false, is_ab = false;
  napi_is_buffer(env, argv[0], &is_buf);
  napi_is_typedarray(env, argv[0], &is_ta);
  napi_is_arraybuffer(env, argv[0], &is_ab);
  if (is_buf) {
    void *d = NULL;
    napi_get_buffer_info(env, argv[0], &d, &mlen);
    msg = malloc(mlen + 1);
    memcpy(msg, d, mlen);
  } else if (is_ta) {
    void *d = NULL;
    size_t alen = 0;
    napi_get_typedarray_info(env, argv[0], NULL, &alen, &d, NULL, NULL);
    mlen = alen;
    msg = malloc(mlen + 1);
    memcpy(msg, d, mlen);
  } else if (is_ab) {
    void *d = NULL;
    size_t alen = 0;
    napi_get_arraybuffer_info(env, argv[0], &d, &alen);
    mlen = alen;
    msg = malloc(mlen + 1);
    memcpy(msg, d, mlen);
  } else {
    size_t slen = 0;
    napi_get_value_string_utf8(env, argv[0], NULL, 0, &slen);
    msg = malloc(slen + 1);
    napi_get_value_string_utf8(env, argv[0], msg, slen + 1, &slen);
    mlen = slen;
  }

  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);

  pthread_mutex_lock(&g_io_mu);
  if (!g_connected) {
    pthread_mutex_unlock(&g_io_mu);
    free(msg);
    napi_throw_error(env, NULL, "not connected");
    return NULL;
  }
  uint8_t lenbuf[4];
  uint32_t be = (uint32_t)mlen;
  lenbuf[0] = (uint8_t)(be & 0xFF);
  lenbuf[1] = (uint8_t)((be >> 8) & 0xFF);
  lenbuf[2] = (uint8_t)((be >> 16) & 0xFF);
  lenbuf[3] = (uint8_t)((be >> 24) & 0xFF);
  int werr = write_all(g_fd, (char *)lenbuf, 4) != 0 ? errno : 0;
  if (werr == 0) werr = write_all(g_fd, msg, mlen) != 0 ? errno : 0;
  free(msg);

  // response: 4-byte LE length + bytes
  uint8_t rl[4];
  size_t got = 0;
  while (got < 4 && werr == 0) {
    ssize_t n = read(g_fd, rl + got, 4 - got);
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
    teardown_connection();
    char msg2[160];
    snprintf(msg2, sizeof(msg2), "response read failed: %s",
             werr == ECONNRESET ? "connection lost" : strerror(werr));
    napi_value mv;
    napi_create_string_utf8(env, msg2, NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    return promise;
  }
  uint32_t rlen = (uint32_t)rl[0] | ((uint32_t)rl[1] << 8) |
                  ((uint32_t)rl[2] << 16) | ((uint32_t)rl[3] << 24);
  char *resp = malloc(rlen + 1);
  got = 0;
  while (got < rlen) {
    ssize_t n = read(g_fd, resp + got, rlen - got);
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
    free(resp);
    teardown_connection();
    char msg2[160];
    snprintf(msg2, sizeof(msg2), "response read failed: %s", strerror(werr));
    napi_value mv;
    napi_create_string_utf8(env, msg2, NAPI_AUTO_LENGTH, &mv);
    napi_reject_deferred(env, deferred, mv);
    return promise;
  }
  pthread_mutex_unlock(&g_io_mu);

  void *copy = NULL;
  napi_value bodyVal;
  napi_create_buffer(env, rlen, &copy, &bodyVal);
  if (copy) memcpy(copy, resp, rlen);
  free(resp);
  napi_resolve_deferred(env, deferred, bodyVal);
  return promise;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "connect", NAPI_AUTO_LENGTH, Connect, NULL, &fn);
  napi_set_named_property(env, exports, "connect", fn);
  napi_create_function(env, "call", NAPI_AUTO_LENGTH, Call, NULL, &fn);
  napi_set_named_property(env, exports, "call", fn);
  napi_create_function(env, "callBin", NAPI_AUTO_LENGTH, CallBin, NULL, &fn);
  napi_set_named_property(env, exports, "callBin", fn);
  napi_create_function(env, "close", NAPI_AUTO_LENGTH, Close, NULL, &fn);
  napi_set_named_property(env, exports, "close", fn);
  napi_create_function(env, "invoke", NAPI_AUTO_LENGTH, Invoke, NULL, &fn);
  napi_set_named_property(env, exports, "invoke", fn);
  return exports;
}

NAPI_MODULE(bridge, Init)
