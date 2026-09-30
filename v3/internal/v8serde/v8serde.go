// Package v8serde parses and produces the V8 ValueSerializer format
// (the wire format of Node's v8.serialize/deserialize), enabling the
// renderer to pass bindings payloads as structured binary instead of
// JSON text. Only the value set reachable from bindings payloads is
// implemented: objects, arrays, strings (one-byte and two-byte),
// int32/uint32/double numbers, booleans, null, undefined and Uint8Array.
// The format is stable across the supported Electron version range
// (see the project decision on binding to a fixed Electron band).
package v8serde

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"unicode/utf16"
	"unicode/utf8"
)

// tags of the V8 ValueSerializer format (value-serializer.h)
const (
	tagVersion     = 0xFF
	tagNull        = 0x30
	tagUndefined   = 0x5F
	tagTrue        = 0x54
	tagFalse       = 0x46
	tagInt32       = 0x49
	tagUint32      = 0x55
	tagDouble      = 0x4E
	tagOneByteStr  = 0x22
	tagTwoByteStr  = 0x63
	tagBeginObject = 0x6F
	tagEndObject   = 0x7B
	tagDenseArray  = 0x41
	tagEndDenseArr = 0x24
	tagUint8Array  = 0x5C
	tagObjectRef   = 0x5E
	tagUnsupported = 0x7F
)

// ErrUnsupported reports a value tag this package does not decode.
var ErrUnsupported = errors.New("v8serde: unsupported value tag")

type decoder struct {
	data []byte
	pos  int
}

func (d *decoder) byte() (byte, error) {
	if d.pos >= len(d.data) {
		return 0, errors.New("v8serde: truncated input")
	}
	b := d.data[d.pos]
	d.pos++
	return b, nil
}

func (d *decoder) bytes(n int) ([]byte, error) {
	if n < 0 || d.pos+n > len(d.data) {
		return nil, errors.New("v8serde: truncated input")
	}
	b := d.data[d.pos : d.pos+n]
	d.pos += n
	return b, nil
}

// readVarint reads an unsigned LEB128 varint.
func (d *decoder) readVarint() (uint64, error) {
	var v uint64
	var shift uint
	for {
		b, err := d.byte()
		if err != nil {
			return 0, err
		}
		v |= uint64(b&0x7F) << shift
		if b < 0x80 {
			return v, nil
		}
		shift += 7
		if shift >= 64 {
			return 0, errors.New("v8serde: varint overflow")
		}
	}
}

// readZigzag reads a signed int32 encoded as zigzag varint.
func (d *decoder) readZigzag() (int32, error) {
	v, err := d.readVarint()
	if err != nil {
		return 0, err
	}
	return int32(v>>1) ^ -int32(v&1), nil
}

// Deserialize parses the output of Node's v8.serialize into Go values:
// objects become map[string]any, dense arrays []any, strings string,
// numbers float64, Uint8Array []byte, booleans bool, null/undefined nil.
func Deserialize(data []byte) (any, error) {
	d := &decoder{data: data}
	if b, err := d.byte(); err != nil {
		return nil, err
	} else if b != tagVersion {
		return nil, fmt.Errorf("v8serde: missing version tag (got %#x)", b)
	}
	if _, err := d.readVarint(); err != nil {
		return nil, err
	}
	return d.value(0)
}

func (d *decoder) value(depth int) (any, error) {
	if depth > 64 {
		return nil, errors.New("v8serde: nesting too deep")
	}
	tag, err := d.byte()
	if err != nil {
		return nil, err
	}
	switch tag {
	case tagNull, tagUndefined:
		return nil, nil
	case tagTrue:
		return true, nil
	case tagFalse:
		return false, nil
	case tagInt32:
		v, err := d.readZigzag()
		if err != nil {
			return nil, err
		}
		return float64(v), nil
	case tagUint32:
		v, err := d.readVarint()
		if err != nil {
			return nil, err
		}
		return float64(v), nil
	case tagDouble:
		b, err := d.bytes(8)
		if err != nil {
			return nil, err
		}
		return math.Float64frombits(binary.LittleEndian.Uint64(b)), nil
	case tagOneByteStr:
		n, err := d.readVarint()
		if err != nil {
			return nil, err
		}
		b, err := d.bytes(int(n))
		if err != nil {
			return nil, err
		}
		// one-byte strings are latin1; widen to UTF-8
		out := make([]byte, 0, n)
		for _, c := range b {
			if c < utf8.RuneSelf {
				out = append(out, c)
			} else {
				r := rune(c)
				out = utf8.AppendRune(out, r)
			}
		}
		return string(out), nil
	case tagTwoByteStr:
		n, err := d.readVarint() // byte count (2 per UTF-16 code unit)
		if err != nil {
			return nil, err
		}
		b, err := d.bytes(int(n))
		if err != nil {
			return nil, err
		}
		u16 := make([]uint16, 0, n/2)
		for i := 0; i+1 < len(b); i += 2 {
			u16 = append(u16, binary.LittleEndian.Uint16(b[i:]))
		}
		return string(utf16.Decode(u16)), nil
	case tagBeginObject:
		obj := map[string]any{}
		for {
			kb, err := d.byte()
			if err != nil {
				return nil, err
			}
			if kb == tagEndObject {
				// the object's numeric id follows (used for references)
				if _, err := d.readVarint(); err != nil {
					return nil, err
				}
				return obj, nil
			}
			// keys are one-byte strings in practice; route generically
			d.pos-- // push back, parse as value
			keyAny, err := d.value(depth + 1)
			if err != nil {
				return nil, err
			}
			key, ok := keyAny.(string)
			if !ok {
				return nil, fmt.Errorf("v8serde: non-string object key (%T)", keyAny)
			}
			val, err := d.value(depth + 1)
			if err != nil {
				return nil, err
			}
			obj[key] = val
		}
	case tagDenseArray:
		n, err := d.readVarint()
		if err != nil {
			return nil, err
		}
		arr := make([]any, 0, n)
		for i := 0; i < int(n); i++ {
			v, err := d.value(depth + 1)
			if err != nil {
				return nil, err
			}
			arr = append(arr, v)
		}
		// dense array terminator: 0x24 + varint(level) + varint(arrayID)
		if b, err := d.byte(); err != nil || b != tagEndDenseArr {
			return nil, errors.New("v8serde: dense array missing terminator")
		}
		if _, err := d.readVarint(); err != nil {
			return nil, err
		}
		if _, err := d.readVarint(); err != nil {
			return nil, err
		}
		return arr, nil
	case tagUint8Array:
		// observed wire form: 0x5C + varint(a) + varint(byteLen) + bytes
		if _, err := d.readVarint(); err != nil {
			return nil, err
		}
		n, err := d.readVarint()
		if err != nil {
			return nil, err
		}
		return d.bytes(int(n))
	case tagObjectRef:
		return nil, fmt.Errorf("v8serde: object references not supported")
	default:
		return nil, fmt.Errorf("v8serde: tag %#x: %w", tag, ErrUnsupported)
	}
}

// Serialize produces V8 ValueSerializer bytes for the supported Go
// values (round-trip counterpart of Deserialize).
func Serialize(v any) ([]byte, error) {
	out := []byte{tagVersion, 0x0F}
	s := &serializer{out: out}
	if err := s.value(v, 0); err != nil {
		return nil, err
	}
	return s.out, nil
}

type serializer struct {
	out     []byte
	idCount uint64
}

// nextID mirrors V8's object-id assignment (monotonic per serialize call).
func (s *serializer) nextID() uint64 { s.idCount++; return s.idCount }

func (s *serializer) byte(b byte) { s.out = append(s.out, b) }
func (s *serializer) varint(v uint64) {
	for v >= 0x80 {
		s.out = append(s.out, byte(v)|0x80)
		v >>= 7
	}
	s.out = append(s.out, byte(v))
}

func (s *serializer) zigzag(v int32) { s.varint(uint64((uint32(v) << 1) ^ uint32(v>>31))) }

func (s *serializer) stringBytes(str string) {
	ascii := true
	for i := 0; i < len(str); i++ {
		if str[i] >= utf8.RuneSelf {
			ascii = false
			break
		}
	}
	if ascii {
		s.byte(tagOneByteStr)
		s.varint(uint64(len(str)))
		s.out = append(s.out, str...)
		return
	}
	// encode as UTF-8 string tag if the runtime emits it, else two-byte;
	// two-byte is what Node's v8.serialize produces for non-latin1
	u16 := utf16.Encode([]rune(str))
	s.byte(tagTwoByteStr)
	s.varint(uint64(len(u16) * 2)) // byte length on the wire
	for _, c := range u16 {
		s.out = binary.LittleEndian.AppendUint16(s.out, c)
	}
}

func (s *serializer) value(v any, depth int) error {
	if depth > 64 {
		return errors.New("v8serde: nesting too deep")
	}
	switch x := v.(type) {
	case nil:
		s.byte(tagNull)
	case bool:
		if x {
			s.byte(tagTrue)
		} else {
			s.byte(tagFalse)
		}
	case string:
		s.stringBytes(x)
	case float64:
		if x == math.Trunc(x) && !math.IsInf(x, 0) && math.Abs(x) < 1<<31 {
			s.byte(tagInt32)
			s.zigzag(int32(x))
		} else {
			s.byte(tagDouble)
			s.out = binary.LittleEndian.AppendUint64(s.out, math.Float64bits(x))
		}
	case []byte:
		s.byte(tagUint8Array)
		s.varint(1)
		s.varint(uint64(len(x)))
		s.out = append(s.out, x...)
	case []any:
		s.byte(tagDenseArray)
		s.varint(uint64(len(x)))
		for _, e := range x {
			if err := s.value(e, depth+1); err != nil {
				return err
			}
		}
		s.byte(tagEndDenseArr)
		s.varint(0) // level
		s.varint(s.nextID())
		s.idCount++
	case map[string]any:
		s.byte(tagBeginObject)
		for k, e := range x {
			s.value(k, depth+1)
			if err := s.value(e, depth+1); err != nil {
				return err
			}
		}
		s.byte(tagEndObject)
		s.varint(s.nextID())
	default:
		return fmt.Errorf("v8serde: unsupported Go type %T", v)
	}
	return nil
}
