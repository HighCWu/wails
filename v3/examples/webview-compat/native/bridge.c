// go-bridge: minimal N-API transport adapter for the webview-compat
// benchmark. Connects a Unix domain socket (per-instance endpoint provided
// by the host config) and exposes an async echo RPC:
//
//   bridge.connect(path)
//   bridge.call(id, payload) -> Promise<string>   // JSONL frame round trip
//   bridge.close()
//
// Benchmark PoC: one libuv async_work per call (write frame, read response
// line). Sequential callers occupy one threadpool thread at a time. The
// production transport upgrade (persistent reader thread + thread-safe
// function dispatch) is deliberately deferred — see the design doc.

#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int bridge_fd = -1;

// ---- connect(path) ---------------------------------------------------------

static napi_value Connect(napi_env env, napi_callback_info info) {
	size_t argc = 2;
	napi_value argv[2];
	napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
	if (argc < 2) {
		napi_throw_error(env, NULL, "connect requires (path, token)");
		return NULL;
	}
	size_t plen = 0;
	napi_get_value_string_utf8(env, argv[0], NULL, 0, &plen);
	char *path = malloc(plen + 1);
	napi_get_value_string_utf8(env, argv[0], path, plen + 1, &plen);
	size_t tlen = 0;
	napi_get_value_string_utf8(env, argv[1], NULL, 0, &tlen);
	char *token = malloc(tlen + 1);
	napi_get_value_string_utf8(env, argv[1], token, tlen + 1, &tlen);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		free(path); free(token);
		napi_throw_error(env, NULL, "socket failed");
		return NULL;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (plen >= sizeof(addr.sun_path)) {
		free(path); free(token); close(fd);
		napi_throw_error(env, NULL, "path too long");
		return NULL;
	}
	memcpy(addr.sun_path, path, plen);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		free(path); free(token); close(fd);
		napi_throw_error(env, NULL, "connect failed");
		return NULL;
	}

	// HELLO handshake: first frame carries the token; the Go side only
	// starts echoing runtime frames after verifying it.
	char hello[256];
	int hl = snprintf(hello, sizeof(hello), "{\"hello\":1,\"token\":\"%s\"}\n", token);
	free(path); free(token);
	size_t off = 0;
	while (off < (size_t)hl) {
		ssize_t n = write(fd, hello + off, hl - off);
		if (n <= 0) { close(fd); bridge_fd = -1; napi_throw_error(env, NULL, "hello write failed"); return NULL; }
		off += (size_t)n;
	}
	char ack[64];
	size_t acklen = 0;
	while (acklen < sizeof(ack) - 1) {
		char c;
		ssize_t n = read(fd, &c, 1);
		if (n <= 0) { close(fd); bridge_fd = -1; napi_throw_error(env, NULL, "handshake read failed"); return NULL; }
		if (c == '\n') break;
		ack[acklen++] = c;
	}
	ack[acklen] = '\0';
	if (strstr(ack, "ready") == NULL) {
		close(fd); bridge_fd = -1;
		napi_throw_error(env, NULL, "handshake rejected");
		return NULL;
	}
	bridge_fd = fd;
	napi_value undef;
	napi_get_undefined(env, &undef);
	return undef;
}

// ---- call(id, payload) -> Promise<string> ----------------------------------

typedef struct {
	// input (owned)
	int fd;
	char *frame;        // full JSONL frame including '\n'
	size_t frame_len;
	// output (filled by execute)
	char *response;     // full line including '\n', NUL terminated
	size_t response_len;
	napi_status status;
	char err[256];
	// async work handle
	napi_async_work work;
	napi_deferred deferred;
} BridgeCall;

static void Execute(napi_env env, void *data) {
	BridgeCall *call = (BridgeCall *)data;
	size_t off = 0;
	while (off < call->frame_len) {
		ssize_t n = write(call->fd, call->frame + off, call->frame_len - off);
		if (n <= 0) { snprintf(call->err, sizeof(call->err), "write failed"); call->status = napi_generic_failure; return; }
		off += (size_t)n;
	}
	size_t cap = 4096, len = 0;
	call->response = malloc(cap);
	if (!call->response) { snprintf(call->err, sizeof(call->err), "oom"); call->status = napi_generic_failure; return; }
	for (;;) {
		char chunk[4096];
		ssize_t n = read(call->fd, chunk, sizeof(chunk));
		if (n < 0) { free(call->response); call->response = NULL; snprintf(call->err, sizeof(call->err), "read failed"); call->status = napi_generic_failure; return; }
		if (n == 0) { free(call->response); call->response = NULL; snprintf(call->err, sizeof(call->err), "go closed connection"); call->status = napi_generic_failure; return; }
		if (len + (size_t)n > cap) { cap = (len + (size_t)n) * 2; call->response = realloc(call->response, cap); }
		memcpy(call->response + len, chunk, (size_t)n);
		len += (size_t)n;
		if (memchr(call->response, '\n', len)) break; // one JSONL frame complete
	}
	call->response[len] = '\0';
	call->response = realloc(call->response, len + 1);
	call->response_len = len;
	call->status = napi_ok;
}

static void Complete(napi_env env, napi_status s, void *data) {
	BridgeCall *call = (BridgeCall *)data;
	napi_value undef, payload, result;
	napi_get_undefined(env, &undef);
	if (call->status == napi_ok && call->response != NULL) {
		// strip trailing '\n' for the resolved string
		napi_create_string_utf8(env, call->response, call->response_len > 0 && call->response[call->response_len - 1] == '\n' ? call->response_len - 1 : call->response_len, &payload);
		napi_resolve_deferred(env, call->deferred, payload);
	} else {
		napi_create_string_utf8(env, call->err, NAPI_AUTO_LENGTH, &payload);
		napi_reject_deferred(env, call->deferred, payload);
	}
	napi_delete_async_work(env, call->work);
	free(call->frame);
	free(call->response);
	free(call);
}

static napi_value Call(napi_env env, napi_callback_info info) {
	size_t argc = 2;
	napi_value argv[2];
	napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
	if (argc < 2 || bridge_fd < 0) {
		napi_throw_error(env, NULL, "call requires connect() first and (id, payload)");
		return NULL;
	}
	double id = 0;
	napi_get_value_double(env, argv[0], &id);
	size_t plen = 0;
	napi_get_value_string_utf8(env, argv[1], NULL, 0, &plen);
	char *payload = malloc(plen + 1);
	napi_get_value_string_utf8(env, argv[1], payload, plen + 1, &plen);

	// frame: {"id":N,"payload":"..."}\n  (payload is JSON-escaped by strlen copy
	// through napi into a JSON string literal built here)
	size_t frame_cap = plen * 6 + 64;
	char *frame = malloc(frame_cap);
	int flen = snprintf(frame, frame_cap, "{\"id\":%d,\"payload\":", (int)id);
	// append payload as a JSON string: escape \ and " and control chars minimally
	for (size_t i = 0; i < plen; i++) {
		char c = payload[i];
		if (c == '"' || c == '\\') { frame[flen++] = '\\'; frame[flen++] = c; }
		else if (c == '\n') { frame[flen++] = '\\'; frame[flen++] = 'n'; }
		else if (c == '\r') { frame[flen++] = '\\'; frame[flen++] = 'r'; }
		else if (c == '\t') { frame[flen++] = '\\'; frame[flen++] = 't'; }
		else frame[flen++] = c;
	}
	frame[flen++] = '"'; frame[flen++] = '}'; frame[flen++] = '\n';
	free(payload);

	napi_value promise, resource_name;
	napi_deferred deferred;
	napi_create_promise(env, &deferred, &promise);

	BridgeCall *call = calloc(1, sizeof(BridgeCall));
	call->fd = bridge_fd;
	call->frame = frame;
	call->frame_len = (size_t)flen;
	napi_create_string_utf8(env, "bridgeCall", NAPI_AUTO_LENGTH, &resource_name);
	napi_create_async_work(env, NULL, resource_name, Execute, Complete, call, &call->work);
	napi_queue_async_work(env, call->work);

	return promise;
}

// ---- close() ---------------------------------------------------------------

static napi_value Close(napi_env env, napi_callback_info info) {
	if (bridge_fd >= 0) { close(bridge_fd); bridge_fd = -1; }
	napi_value undef;
	napi_get_undefined(env, &undef);
	return undef;
}

// ---- module init ------------------------------------------------------------

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

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
