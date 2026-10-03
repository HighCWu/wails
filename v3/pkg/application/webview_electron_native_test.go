package application

// The bridge endpoint's HELLO handshake is the only thing standing
// between the app's webviews and arbitrary local processes: a connection
// with the wrong token must be dropped without a ready frame, while the
// correct token gets one.

import (
	"net"
	"net/http"
	"path/filepath"
	"testing"
	"time"
)

func dialAndHandshake(t *testing.T, sock, token string) (string, error) {
	t.Helper()
	conn, err := net.Dial("unix", sock)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	if err := writeMessage(conn, []byte(`{"hello":1,"token":"` + token + `"}`)); err != nil {
		t.Fatalf("write: %v", err)
	}
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	buf := make([]byte, 0, 256)
	msg, err := readMessage(conn, &buf)
	if err != nil {
		return "", err
	}
	return string(msg), nil
}

func TestNativeBridgeTokenHandshake(t *testing.T) {
	sock := filepath.Join(t.TempDir(), "bridge.sock")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Skipf("unix sockets unavailable: %v", err)
	}
	defer ln.Close()
	serveBridgeListener(ln, "secret-token", func(method, rawURL, body string, hdr http.Header) (int, string, []byte, error) {
		return http.StatusOK, "text/plain", nil, nil
	})

	// wrong token: dropped silently — no ready frame, connection closed
	if msg, err := dialAndHandshake(t, sock, "wrong-token"); err == nil {
		t.Fatalf("wrong token got a reply: %q", msg)
	}

	// correct token: ready frame
	msg, err := dialAndHandshake(t, sock, "secret-token")
	if err != nil {
		t.Fatalf("correct token handshake failed: %v", err)
	}
	if msg != `{"ready":true}` {
		t.Fatalf("unexpected ready frame: %q", msg)
	}
}
