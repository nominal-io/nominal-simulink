// Package frame decodes the host-to-proxy wire format.
//
// This is a deliberate second implementation. The format is specified in
// cstream/src/nm_frame.h and encoded by cstream/src/nm_frame.c; nothing here shares code
// with those, and the two are held together only by the golden fixtures in
// cstream/tests/fixtures/frames.txt.
//
// That is the point. A protocol meant to be implemented by third parties — in LabVIEW,
// in Python, in whatever a customer already runs — has to be followable from its
// specification alone. Writing it twice is how we find out whether it is, and it already
// paid for itself: the C side's config_id hash had a transcribed digit missing, which a
// second implementation using the real FNV-1a constant exposed.
package frame

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/fnv"
	"math"
)

// Magic is checked before anything else is trusted.
//
// A byte sequence rather than an integer: written as a little-endian uint32 it would
// appear as "1SMN" in a packet capture, and being recognisable in tcpdump is a magic
// number's main job.
var Magic = [4]byte{'N', 'M', 'S', '1'}

const (
	Version uint8 = 1

	// MaxBytes bounds a datagram so IP never fragments it. One lost fragment would
	// cost the whole frame, which is worse than sending more datagrams.
	MaxBytes = 1400

	HeaderBytes    = 24
	DataFixedBytes = 12 // sequence(8) + channelCount(2) + rowCount(2)

	// MaxChannels is derived from the MTU, not chosen: a row is 8 bytes of timestamp
	// plus 8 per channel, and must fit alongside the header and fixed DATA fields.
	MaxChannels = (MaxBytes - HeaderBytes - DataFixedBytes - 8) / 8
)

// Frame types.
const (
	TypeHello uint8 = 1
	TypeData  uint8 = 2
)

// ErrWrongVersion is returned separately from other malformed input because it is the
// one failure an operator can act on, and because there is no back-channel to tell the
// host about it — a version mismatch otherwise looks exactly like a network fault.
var (
	ErrShort        = errors.New("datagram shorter than the header")
	ErrMagic        = errors.New("wrong magic")
	ErrWrongVersion = errors.New("unsupported protocol version")
	ErrType         = errors.New("unknown frame type")
	ErrMalformed    = errors.New("malformed frame body")
)

// Header is common to both frame types, so a receiver can dispatch before knowing
// which it holds.
type Header struct {
	Version   uint8
	Type      uint8
	SessionID uint64
	ConfigID  uint64
}

// Hello carries the channel mapping. It repeats roughly every second rather than being
// sent once: UDP is lossy, and a proxy that restarts mid-run has no way to ask for the
// mapping again.
type Hello struct {
	Header
	DatasetRID string
	Channels   []string
}

// Data carries rows that reference channels by index into the Hello list, which is why
// a row costs 8 bytes per channel and no strings appear on the hot path.
type Data struct {
	Header
	Sequence     uint64
	ChannelCount int
	Rows         []Row
}

// Row is one sample across every channel, sharing a timestamp.
type Row struct {
	TimestampNanos int64
	Values         []float64
}

// ConfigID hashes the dataset RID and ordered channel names.
//
// FNV-1a over each field length-prefixed as a 4-byte little-endian count followed by
// its bytes. The length prefixes are what stop {"ab","c"} and {"a","bc"} colliding.
// A sender that computes a different value has its HELLO rejected with a warning, and
// every DATA frame is dropped as an unknown config. This must match the C side exactly.
// The fixture test checks it.
func ConfigID(datasetRID string, channels []string) uint64 {
	h := fnv.New64a()
	writeField(h, datasetRID)
	for _, name := range channels {
		writeField(h, name)
	}
	return h.Sum64()
}

func writeField(h interface{ Write([]byte) (int, error) }, s string) {
	var lenLE [4]byte
	binary.LittleEndian.PutUint32(lenLE[:], uint32(len(s)))
	_, _ = h.Write(lenLE[:])
	_, _ = h.Write([]byte(s))
}

// DecodeHeader validates the framing and reads the common fields. Cheap enough to run
// before deciding whether a datagram is worth caring about.
func DecodeHeader(buf []byte) (Header, error) {
	var h Header

	if len(buf) < HeaderBytes {
		return h, ErrShort
	}
	if !bytes.Equal(buf[0:4], Magic[:]) {
		return h, ErrMagic
	}
	if buf[4] != Version {
		return h, fmt.Errorf("%w: %d (this proxy speaks %d)", ErrWrongVersion, buf[4], Version)
	}
	if buf[5] != TypeHello && buf[5] != TypeData {
		return h, ErrType
	}

	h.Version = buf[4]
	h.Type = buf[5]
	h.SessionID = binary.LittleEndian.Uint64(buf[8:16])
	h.ConfigID = binary.LittleEndian.Uint64(buf[16:24])
	return h, nil
}

// DecodeHello parses a HELLO body.
//
// Every length is checked against the remaining bytes. These datagrams arrive
// unauthenticated from the network, so a truncated or hostile frame has to produce an
// error rather than a panic — Go would turn an unchecked slice into a runtime panic and
// take the proxy down, which is a denial of service anyone on the segment could trigger.
func DecodeHello(buf []byte) (*Hello, error) {
	h, err := DecodeHeader(buf)
	if err != nil {
		return nil, err
	}
	if h.Type != TypeHello {
		return nil, ErrType
	}

	at := HeaderBytes
	rid, at, err := readString(buf, at)
	if err != nil {
		return nil, err
	}

	if at+2 > len(buf) {
		return nil, ErrMalformed
	}
	count := int(binary.LittleEndian.Uint16(buf[at : at+2]))
	at += 2
	if count == 0 || count > MaxChannels {
		return nil, fmt.Errorf("%w: channel count %d", ErrMalformed, count)
	}

	channels := make([]string, 0, count)
	for i := 0; i < count; i++ {
		var name string
		name, at, err = readString(buf, at)
		if err != nil {
			return nil, err
		}
		channels = append(channels, name)
	}

	return &Hello{Header: h, DatasetRID: rid, Channels: channels}, nil
}

// DecodeData parses a DATA body.
func DecodeData(buf []byte) (*Data, error) {
	h, err := DecodeHeader(buf)
	if err != nil {
		return nil, err
	}
	if h.Type != TypeData {
		return nil, ErrType
	}

	at := HeaderBytes
	if at+DataFixedBytes > len(buf) {
		return nil, ErrMalformed
	}
	seq := binary.LittleEndian.Uint64(buf[at : at+8])
	at += 8
	nch := int(binary.LittleEndian.Uint16(buf[at : at+2]))
	at += 2
	rows := int(binary.LittleEndian.Uint16(buf[at : at+2]))
	at += 2

	if nch == 0 || nch > MaxChannels || rows == 0 {
		return nil, fmt.Errorf("%w: %d channels, %d rows", ErrMalformed, nch, rows)
	}
	rowBytes := 8 + 8*nch
	if at+rows*rowBytes > len(buf) {
		return nil, ErrMalformed
	}

	d := &Data{Header: h, Sequence: seq, ChannelCount: nch, Rows: make([]Row, rows)}
	for r := 0; r < rows; r++ {
		p := at + r*rowBytes
		values := make([]float64, nch)
		for c := 0; c < nch; c++ {
			bits := binary.LittleEndian.Uint64(buf[p+8+8*c : p+16+8*c])
			values[c] = math.Float64frombits(bits)
		}
		d.Rows[r] = Row{
			TimestampNanos: int64(binary.LittleEndian.Uint64(buf[p : p+8])),
			Values:         values,
		}
	}
	return d, nil
}

// readString reads a 2-byte little-endian length and that many bytes.
func readString(buf []byte, at int) (string, int, error) {
	if at+2 > len(buf) {
		return "", at, ErrMalformed
	}
	n := int(binary.LittleEndian.Uint16(buf[at : at+2]))
	at += 2
	if at+n > len(buf) {
		return "", at, ErrMalformed
	}
	return string(buf[at : at+n]), at + n, nil
}
