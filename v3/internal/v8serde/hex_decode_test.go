package v8serde

import (
	"encoding/hex"
	"testing"
)

func TestDecodeChineseRound(t *testing.T) {
	// bytes produced by Electron's node: v8.serialize(["value","中文输入"])
	raw, _ := hex.DecodeString("ff104102220576616c75650063082d4e8765938f6551240002")
	v, err := Deserialize(raw)
	if err != nil {
		t.Fatalf("array: %v", err)
	}
	t.Logf("array: %#v", v)
	raw2, _ := hex.DecodeString("ff106f22066f626a656374490022066d6574686f6449002204617267734102220576616c756563082d4e8765938f65512400027b03")
	v2, err := Deserialize(raw2)
	if err != nil {
		t.Fatalf("object: %v", err)
	}
	t.Logf("object: %#v", v2)
}
