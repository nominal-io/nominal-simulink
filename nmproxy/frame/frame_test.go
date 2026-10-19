package frame

import (
	"bufio"
	"encoding/hex"
	"hash/fnv"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// The fixtures are produced by the C encoder (`just frames`). Nothing in this package
// shares code with it, so agreeing on these bytes is the only evidence the two
// implementations actually interoperate.
const fixturePath = "../../cstream/tests/fixtures/frames.txt"

// Values the fixture is documented to contain. Written out longhand rather than derived,
// because a test that computes its expectations the same way the code does will happily
// confirm a shared mistake.
const (
	wantRID       = "ri.catalog.test.dataset.abc"
	wantSessionID = uint64(0x1122334455667788)
	wantConfigID  = uint64(0x5bbb5bdfcbf6bf55)
	wantSequence  = uint64(42)
)

var (
	wantChannels = []string{"engine.rpm", "engine.egt", "engine.psi"}
	wantTags     = []Tag{{"model", "A"}, {"execution-id", "7"}}
	wantTimes    = []int64{1000000000, 1000001000, 1000002000}
	wantValues   = [][]float64{
		{1500.0, 700.0, 30.0},
		{1510.5, 701.5, 30.1},
		{1521.25, 703.0, 30.2},
	}
)

// loadFixtures reads the `key hex` lines, ignoring comments and blanks.
func loadFixtures(t *testing.T) map[string][]byte {
	t.Helper()

	f, err := os.Open(filepath.FromSlash(fixturePath))
	if err != nil {
		t.Fatalf("cannot open %s: %v", fixturePath, err)
	}
	defer f.Close()

	out := map[string][]byte{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Fields(line)
		if len(parts) != 2 {
			continue
		}
		raw, err := hex.DecodeString(parts[1])
		if err != nil {
			// Not every line is hex — max_channels is a decimal, for instance.
			out[parts[0]] = nil
			continue
		}
		out[parts[0]] = raw
	}
	if err := sc.Err(); err != nil {
		t.Fatalf("reading %s: %v", fixturePath, err)
	}
	return out
}

// TestConfigIDMatchesC is the test that matters most. If Go and C disagree here, the
// proxy rejects every HELLO from a C host and no data arrives. It is also the check that
// caught a transcribed digit missing from the C side's FNV-1a offset basis.
func TestConfigIDMatchesC(t *testing.T) {
	got := ConfigID(wantRID, wantChannels, wantTags)
	if got != wantConfigID {
		t.Fatalf("ConfigID = %#016x, C encoder produced %#016x", got, wantConfigID)
	}

	// Two runs of one model differ only in their tags, and must not share a config.
	next := []Tag{{"model", "A"}, {"execution-id", "8"}}
	if ConfigID(wantRID, wantChannels, next) == got {
		t.Error("ConfigID ignores a changed tag value")
	}
}

// TestFNV1AVector guards the hash itself against the standard, independently of our own
// fixtures. hash/fnv is the stdlib implementation, so this mostly documents the vector a
// third-party implementer should check first.
func TestFNV1AVector(t *testing.T) {
	// FNV-1a/64 of the single byte 'a'.
	const want = uint64(0xaf63dc4c8601ec8c)

	h := fnv.New64a()
	_, _ = h.Write([]byte("a"))
	if got := h.Sum64(); got != want {
		t.Fatalf("FNV-1a(\"a\") = %#016x, want %#016x", got, want)
	}
}

func TestDecodeHelloFixture(t *testing.T) {
	fx := loadFixtures(t)
	buf := fx["hello"]
	if buf == nil {
		t.Fatal("no hello fixture")
	}

	h, err := DecodeHello(buf)
	if err != nil {
		t.Fatalf("DecodeHello: %v", err)
	}
	if h.SessionID != wantSessionID {
		t.Errorf("SessionID = %#016x, want %#016x", h.SessionID, wantSessionID)
	}
	if h.ConfigID != wantConfigID {
		t.Errorf("ConfigID = %#016x, want %#016x", h.ConfigID, wantConfigID)
	}
	if h.DatasetRID != wantRID {
		t.Errorf("DatasetRID = %q, want %q", h.DatasetRID, wantRID)
	}
	if len(h.Channels) != len(wantChannels) {
		t.Fatalf("got %d channels, want %d", len(h.Channels), len(wantChannels))
	}
	for i, name := range wantChannels {
		if h.Channels[i] != name {
			t.Errorf("channel %d = %q, want %q", i, h.Channels[i], name)
		}
	}
	if len(h.Tags) != len(wantTags) {
		t.Fatalf("got %d tags, want %d", len(h.Tags), len(wantTags))
	}
	for i, tag := range wantTags {
		if h.Tags[i] != tag {
			t.Errorf("tag %d = %+v, want %+v", i, h.Tags[i], tag)
		}
	}

	// The mapping only works if the announced channels hash to the announced config,
	// so verify the HELLO is internally consistent as a proxy would.
	if ConfigID(h.DatasetRID, h.Channels, h.Tags) != h.ConfigID {
		t.Error("recomputing ConfigID from the HELLO contents disagrees with its header")
	}
}

func TestDecodeDataFixture(t *testing.T) {
	fx := loadFixtures(t)
	buf := fx["data"]
	if buf == nil {
		t.Fatal("no data fixture")
	}

	d, err := DecodeData(buf)
	if err != nil {
		t.Fatalf("DecodeData: %v", err)
	}
	if d.Sequence != wantSequence {
		t.Errorf("Sequence = %d, want %d", d.Sequence, wantSequence)
	}
	if d.ChannelCount != len(wantChannels) {
		t.Fatalf("ChannelCount = %d, want %d", d.ChannelCount, len(wantChannels))
	}
	if len(d.Rows) != len(wantTimes) {
		t.Fatalf("got %d rows, want %d", len(d.Rows), len(wantTimes))
	}

	for r, row := range d.Rows {
		if row.TimestampNanos != wantTimes[r] {
			t.Errorf("row %d timestamp = %d, want %d", r, row.TimestampNanos, wantTimes[r])
		}
		for c, want := range wantValues[r] {
			// The fixture's 30.1 and 30.2 are not exactly representable, so compare
			// against the same literal rather than with a tolerance — the bytes should
			// round-trip bit-for-bit, and a tolerance would hide a real encoding bug.
			if row.Values[c] != want {
				t.Errorf("row %d value %d = %v (bits %#016x), want %v (bits %#016x)",
					r, c, row.Values[c], math.Float64bits(row.Values[c]),
					want, math.Float64bits(want))
			}
		}
	}
}

func TestMaxChannelsMatchesC(t *testing.T) {
	fx := loadFixtures(t)
	// max_channels is decimal, so loadFixtures stored nil; re-read it as a number.
	f, err := os.Open(filepath.FromSlash(fixturePath))
	if err != nil {
		t.Fatalf("cannot open fixtures: %v", err)
	}
	defer f.Close()

	var fromC int
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		parts := strings.Fields(strings.TrimSpace(sc.Text()))
		if len(parts) == 2 && parts[0] == "max_channels" {
			fromC, _ = strconv.Atoi(parts[1])
		}
	}
	if fromC == 0 {
		t.Skip("fixture does not record max_channels")
	}
	if MaxChannels != fromC {
		t.Errorf("MaxChannels = %d, C derives %d — the MTU derivation has diverged",
			MaxChannels, fromC)
	}
	_ = fx
}

// TestRejectsMalformed is the security-relevant half. These datagrams arrive
// unauthenticated, and Go turns an unchecked slice index into a panic — so a frame that
// claims more than it carries would let anyone on the segment kill the proxy.
func TestRejectsMalformed(t *testing.T) {
	fx := loadFixtures(t)

	for _, name := range []string{"hello", "data"} {
		buf := fx[name]
		if buf == nil {
			t.Fatalf("no %s fixture", name)
		}

		// Truncated at every length, not just one.
		for n := 0; n < len(buf); n++ {
			var err error
			if name == "hello" {
				_, err = DecodeHello(buf[:n])
			} else {
				_, err = DecodeData(buf[:n])
			}
			if err == nil {
				t.Errorf("%s truncated to %d bytes was accepted", name, n)
			}
		}

		// Corrupted framing.
		for _, tc := range []struct {
			what   string
			mutate func([]byte)
			want   error
		}{
			{"bad magic", func(b []byte) { b[0] ^= 0xFF }, ErrMagic},
			{"unknown version", func(b []byte) { b[4] = 99 }, ErrWrongVersion},
			{"unknown type", func(b []byte) { b[5] = 77 }, ErrType},
		} {
			bad := make([]byte, len(buf))
			copy(bad, buf)
			tc.mutate(bad)

			if _, err := DecodeHeader(bad); err == nil {
				t.Errorf("%s: %s was accepted", name, tc.what)
			}
		}

		// A wrong-type decode must be refused, not silently misread.
		if name == "hello" {
			if _, err := DecodeData(buf); err == nil {
				t.Error("a HELLO decoded as DATA")
			}
		} else {
			if _, err := DecodeHello(buf); err == nil {
				t.Error("a DATA decoded as HELLO")
			}
		}
	}
}

// TestInflatedCounts covers the case the truncation loop cannot: a full-length frame
// whose internal counts claim more than the bytes provide.
func TestInflatedCounts(t *testing.T) {
	fx := loadFixtures(t)

	hello := make([]byte, len(fx["hello"]))
	copy(hello, fx["hello"])
	// channel_count sits after the header, the 2-byte rid length and the rid itself.
	at := HeaderBytes + 2 + len(wantRID)
	hello[at], hello[at+1] = 0xFF, 0x00
	if _, err := DecodeHello(hello); err == nil {
		t.Error("HELLO with an inflated channel count was accepted")
	}

	// tag_count sits after the channel names.
	at = HeaderBytes + 2 + len(wantRID) + 2
	for _, name := range wantChannels {
		at += 2 + len(name)
	}
	copy(hello, fx["hello"])
	hello[at], hello[at+1] = byte(len(wantTags)+1), 0x00
	if _, err := DecodeHello(hello); err == nil {
		t.Error("HELLO with an inflated tag count was accepted")
	}

	// The first tag's key with its length zeroed must not decode as a tag with no key.
	copy(hello, fx["hello"])
	hello[at+2], hello[at+3] = 0x00, 0x00
	if _, err := DecodeHello(hello); err == nil {
		t.Error("HELLO with an empty tag key was accepted")
	}

	data := make([]byte, len(fx["data"]))
	copy(data, fx["data"])
	// row_count sits after the header, sequence(8) and channel_count(2).
	at = HeaderBytes + 10
	data[at], data[at+1] = 0xFF, 0xFF
	if _, err := DecodeData(data); err == nil {
		t.Error("DATA with an inflated row count was accepted")
	}
}
