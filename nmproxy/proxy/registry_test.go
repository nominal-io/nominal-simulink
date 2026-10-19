package proxy

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/nominal-io/nominal-simulink/nmproxy/frame"
)

// Frames come from the C encoder's fixtures rather than a Go encoder, because the proxy
// never encodes — a Go encoder written only for tests would be one more implementation
// to keep correct. A test that needs another sequence number uses withSequence, which
// copies the DATA fixture and overwrites its 8-byte sequence field.
const fixturePath = "../../cstream/tests/fixtures/frames.txt"

const fixtureRID = "ri.catalog.test.dataset.abc"

func loadFixture(t *testing.T, key string) []byte {
	t.Helper()

	f, err := os.Open(filepath.FromSlash(fixturePath))
	if err != nil {
		t.Fatalf("cannot open %s: %v", fixturePath, err)
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	for sc.Scan() {
		parts := strings.Fields(strings.TrimSpace(sc.Text()))
		if len(parts) == 2 && parts[0] == key {
			raw, err := hex.DecodeString(parts[1])
			if err != nil {
				t.Fatalf("fixture %q is not hex: %v", key, err)
			}
			return raw
		}
	}
	t.Fatalf("no %q fixture in %s", key, fixturePath)
	return nil
}

// withSequence returns a copy of a DATA frame carrying a different sequence number.
// The field sits immediately after the header, per the ABNF in nm_frame.h.
func withSequence(data []byte, seq uint64) []byte {
	out := make([]byte, len(data))
	copy(out, data)
	binary.LittleEndian.PutUint64(out[frame.HeaderBytes:frame.HeaderBytes+8], seq)
	return out
}

// withRID returns a copy of a HELLO frame for another dataset, with its config_id
// recomputed so the proxy accepts it. The new RID must be as long as the old one, so the
// frame's lengths stay valid.
func withRID(t *testing.T, hello []byte, rid string) []byte {
	t.Helper()
	if len(rid) != len(fixtureRID) {
		t.Fatalf("withRID needs a %d-byte RID, got %q", len(fixtureRID), rid)
	}
	h, err := frame.DecodeHello(hello)
	if err != nil {
		t.Fatalf("decoding the HELLO fixture: %v", err)
	}
	out := make([]byte, len(hello))
	copy(out, hello)
	// The RID follows the header and its own 2-byte length.
	copy(out[frame.HeaderBytes+2:], rid)
	binary.LittleEndian.PutUint64(out[16:24], frame.ConfigID(rid, h.Channels, h.Tags))
	return out
}

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, &slog.HandlerOptions{Level: slog.LevelError}))
}

// logRecorder is a slog handler that keeps every message, so a test can count how often
// a line was logged.
type logRecorder struct {
	mu   sync.Mutex
	msgs []string
}

func (l *logRecorder) Enabled(context.Context, slog.Level) bool { return true }
func (l *logRecorder) WithAttrs([]slog.Attr) slog.Handler       { return l }
func (l *logRecorder) WithGroup(string) slog.Handler            { return l }

func (l *logRecorder) Handle(_ context.Context, r slog.Record) error {
	l.mu.Lock()
	l.msgs = append(l.msgs, r.Message)
	l.mu.Unlock()
	return nil
}

// count reports how many logged messages start with prefix.
func (l *logRecorder) count(prefix string) int {
	l.mu.Lock()
	defer l.mu.Unlock()
	n := 0
	for _, m := range l.msgs {
		if strings.HasPrefix(m, prefix) {
			n++
		}
	}
	return n
}

// countExact reports how many logged messages are exactly msg.
func (l *logRecorder) countExact(msg string) int {
	l.mu.Lock()
	defer l.mu.Unlock()
	n := 0
	for _, m := range l.msgs {
		if m == msg {
			n++
		}
	}
	return n
}

func newTestRegistry(t *testing.T, cfg Config) (*Registry, *LogWriter) {
	t.Helper()
	w := &LogWriter{Log: quietLogger()}
	if cfg.IdleTimeout == 0 {
		cfg.IdleTimeout = time.Minute
	}
	return NewRegistry(w, cfg, quietLogger()), w
}

func TestHelloThenDataWrites(t *testing.T) {
	r, w := newTestRegistry(t, Config{})
	now := time.Now()

	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)

	if got := w.Rows(); got != 3 {
		t.Errorf("wrote %d rows, want 3", got)
	}
	if st := r.Stats(); st.Configs != 1 || st.Sessions != 1 {
		t.Errorf("configs=%d sessions=%d, want 1 and 1", st.Configs, st.Sessions)
	}
}

// Data arriving before its HELLO is the normal state of affairs after a proxy restart,
// so it must be counted and shrugged off rather than treated as an error.
func TestDataBeforeHelloIsDroppedNotFatal(t *testing.T) {
	r, w := newTestRegistry(t, Config{})
	now := time.Now()

	r.Handle(loadFixture(t, "data"), now)

	if got := w.Rows(); got != 0 {
		t.Errorf("wrote %d rows before any HELLO, want 0", got)
	}
	st := r.Stats()
	if st.UnknownConfig != 1 {
		t.Errorf("unknown_config=%d, want 1", st.UnknownConfig)
	}
	if st.BadFrames != 0 {
		t.Errorf("bad_frames=%d — an unknown config is not a malformed frame", st.BadFrames)
	}

	// And once the HELLO lands, the following data flows.
	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)
	if got := w.Rows(); got != 3 {
		t.Errorf("wrote %d rows after the HELLO, want 3", got)
	}
}

// Sequence gaps are the only visibility into loss, since the host cannot know a
// datagram vanished. Getting the arithmetic wrong would either hide loss or invent it.
func TestSequenceGapsAndReordering(t *testing.T) {
	data := loadFixture(t, "data")

	cases := []struct {
		name         string
		sequences    []uint64
		wantGaps     uint64
		wantReorders uint64
	}{
		{
			name:      "contiguous",
			sequences: []uint64{0, 1, 2, 3},
		},
		{
			// The first sequence seen sets the baseline. A proxy that starts mid-run
			// must not report every frame before it as lost.
			name:      "starting mid-run is not loss",
			sequences: []uint64{5000, 5001, 5002},
		},
		{
			name:      "one missing frame",
			sequences: []uint64{0, 1, 3, 4},
			wantGaps:  1,
		},
		{
			// Counting the difference rather than incrementing by one is what makes
			// the number meaningful when a burst is lost.
			name:      "a burst of loss counts every frame",
			sequences: []uint64{0, 100},
			wantGaps:  99,
		},
		{
			// UDP may reorder. The frame arriving late was already counted as a gap
			// when its successor arrived, so counting it again would double-count.
			name:         "reordering is not loss",
			sequences:    []uint64{0, 2, 1, 3},
			wantGaps:     1,
			wantReorders: 1,
		},
		{
			name:         "a duplicate is a reorder, not a gap",
			sequences:    []uint64{0, 1, 1, 2},
			wantReorders: 1,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			r, _ := newTestRegistry(t, Config{})
			now := time.Now()
			r.Handle(loadFixture(t, "hello"), now)

			for _, seq := range tc.sequences {
				r.Handle(withSequence(data, seq), now)
			}

			st := r.Stats()
			if st.Gaps != tc.wantGaps {
				t.Errorf("gaps=%d, want %d", st.Gaps, tc.wantGaps)
			}
			if st.Reorders != tc.wantReorders {
				t.Errorf("reorders=%d, want %d", st.Reorders, tc.wantReorders)
			}
			// Every frame's rows are written regardless of order: a reordered frame's
			// data is still good, and dropping it would turn a harmless network
			// artifact into real loss.
			if wantRows := len(tc.sequences) * 3; int(st.Rows) != wantRows {
				t.Errorf("rows=%d, want %d", st.Rows, wantRows)
			}
		})
	}
}

func TestAllowlistRefusesUnlistedDatasets(t *testing.T) {
	r, w := newTestRegistry(t, Config{
		AllowedDatasets: []string{"ri.catalog.test.dataset.something-else"},
	})
	now := time.Now()

	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)

	if got := w.Rows(); got != 0 {
		t.Errorf("wrote %d rows for a dataset not on the allowlist, want 0", got)
	}
	if st := r.Stats(); st.Refused != 1 {
		t.Errorf("refused=%d, want 1", st.Refused)
	}
}

// Every refused dataset is named once, not only the first one the proxy ever saw, and
// not once per HELLO.
func TestRefusedDatasetsAreLoggedOncePerRID(t *testing.T) {
	rec := &logRecorder{}
	r := NewRegistry(&LogWriter{Log: quietLogger()}, Config{
		IdleTimeout:     time.Minute,
		AllowedDatasets: []string{"ri.catalog.test.dataset.something-else"},
	}, slog.New(rec))
	now := time.Now()

	abc := loadFixture(t, "hello")
	xyz := withRID(t, abc, "ri.catalog.test.dataset.xyz")
	for i := 0; i < 3; i++ {
		r.Handle(abc, now)
		r.Handle(xyz, now)
	}

	if got := rec.count("refusing a dataset"); got != 2 {
		t.Errorf("logged %d refusals for 2 datasets, want 2", got)
	}
	if st := r.Stats(); st.Refused != 6 {
		t.Errorf("refused=%d, want 6", st.Refused)
	}
}

// Any sender can invent RIDs, so the number named in the log is capped.
func TestRefusedDatasetLoggingIsCapped(t *testing.T) {
	rec := &logRecorder{}
	r := NewRegistry(&LogWriter{Log: quietLogger()}, Config{
		IdleTimeout:     time.Minute,
		AllowedDatasets: []string{"ri.catalog.test.dataset.something-else"},
	}, slog.New(rec))
	now := time.Now()

	hello := loadFixture(t, "hello")
	for i := 0; i < maxRefusedRIDsLogged+20; i++ {
		r.Handle(withRID(t, hello, fmt.Sprintf("ri.catalog.test.dataset.%03d", i)), now)
	}

	if got := rec.count("refusing a dataset"); got != maxRefusedRIDsLogged {
		t.Errorf("logged %d refusals, want the cap of %d", got, maxRefusedRIDsLogged)
	}
}

func TestAllowlistAdmitsListedDatasets(t *testing.T) {
	r, w := newTestRegistry(t, Config{AllowedDatasets: []string{fixtureRID}})
	now := time.Now()

	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)

	if got := w.Rows(); got != 3 {
		t.Errorf("wrote %d rows, want 3", got)
	}
}

// A repeated HELLO is the common case — hosts re-announce about once a second — so it
// must refresh rather than open a second stream for the same configuration.
func TestRepeatedHelloDoesNotReopen(t *testing.T) {
	r, _ := newTestRegistry(t, Config{})
	now := time.Now()
	hello := loadFixture(t, "hello")

	for i := 0; i < 10; i++ {
		r.Handle(hello, now.Add(time.Duration(i)*time.Second))
	}

	if st := r.Stats(); st.Configs != 1 {
		t.Errorf("configs=%d after ten HELLOs, want 1", st.Configs)
	}
}

func TestSweepEvictsIdleAndFlushes(t *testing.T) {
	r, _ := newTestRegistry(t, Config{IdleTimeout: 10 * time.Second})
	start := time.Now()

	r.Handle(loadFixture(t, "hello"), start)
	r.Handle(loadFixture(t, "data"), start)

	r.Sweep(start.Add(5 * time.Second))
	if st := r.Stats(); st.Configs != 1 || st.Sessions != 1 {
		t.Fatalf("swept too early: configs=%d sessions=%d", st.Configs, st.Sessions)
	}

	r.Sweep(start.Add(30 * time.Second))
	st := r.Stats()
	if st.Configs != 0 || st.Sessions != 0 {
		t.Errorf("configs=%d sessions=%d after the idle timeout, want 0 and 0",
			st.Configs, st.Sessions)
	}

	// The periodic log subtracts consecutive snapshots, so a total that fell when its
	// session was forgotten printed as a delta of about 1.8e19.
	if st.Rows != 3 {
		t.Errorf("rows=%d after eviction, want the 3 the session wrote", st.Rows)
	}
}

// asyncFailWriter hands back the error callback so a test can fail a write after Push
// has returned, the way the Nominal client does.
type asyncFailWriter struct {
	LogWriter
	onError func(error)
}

func (w *asyncFailWriter) Open(rid string, channels []string, tags map[string]string,
	onError func(error)) (Stream, error) {
	w.onError = onError
	return w.LogWriter.Open(rid, channels, tags, nil)
}

// The Nominal client only enqueues on Push and reports failures later, so a bad token
// once showed write_errors=0 while every request failed.
func TestWritesThatFailLaterAreCounted(t *testing.T) {
	w := &asyncFailWriter{LogWriter: LogWriter{Log: quietLogger()}}
	r := NewRegistry(w, Config{IdleTimeout: time.Minute}, quietLogger())
	now := time.Now()

	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)
	if w.onError == nil {
		t.Fatal("the registry gave the writer no way to report a later failure")
	}

	w.onError(errors.New("401 Unauthorized"))
	w.onError(errors.New("401 Unauthorized"))
	if st := r.Stats(); st.WriteErrors != 2 {
		t.Errorf("write_errors=%d, want 2", st.WriteErrors)
	}
}

// tagWriter keeps the tags each stream was opened with.
type tagWriter struct {
	LogWriter
	tags map[string]string
}

func (w *tagWriter) Open(rid string, channels []string, tags map[string]string,
	onError func(error)) (Stream, error) {
	w.tags = tags
	return w.LogWriter.Open(rid, channels, tags, onError)
}

// The tags in a HELLO are what keep one execution's rows apart from another's in a shared
// dataset, so they have to reach the stream.
func TestHelloTagsReachTheWriter(t *testing.T) {
	w := &tagWriter{LogWriter: LogWriter{Log: quietLogger()}}
	r := NewRegistry(w, Config{IdleTimeout: time.Minute}, quietLogger())

	r.Handle(loadFixture(t, "hello"), time.Now())
	want := map[string]string{"model": "A", "execution-id": "7"}
	if len(w.tags) != len(want) {
		t.Fatalf("stream opened with tags %v, want %v", w.tags, want)
	}
	for k, v := range want {
		if w.tags[k] != v {
			t.Errorf("tag %s = %q, want %q", k, w.tags[k], v)
		}
	}
}

// Rubbish on the port must not be able to stop the proxy. Anyone who can reach it can
// send anything, so every malformed input has to be a counter rather than a panic.
func TestGarbageIsCountedNotFatal(t *testing.T) {
	r, w := newTestRegistry(t, Config{})
	now := time.Now()

	inputs := [][]byte{
		{},
		{0x00},
		make([]byte, frame.HeaderBytes),          // right length, wrong magic
		[]byte("this is not a frame at all, it"), // plausible length, no structure
	}
	for _, in := range inputs {
		r.Handle(in, now)
	}

	// A version we do not speak, which must be distinguished from generic garbage so an
	// operator can see the mismatch.
	wrongVersion := loadFixture(t, "hello")
	wrongVersion[4] = 99
	r.Handle(wrongVersion, now)

	st := r.Stats()
	if st.WrongVersion != 1 {
		t.Errorf("wrong_version=%d, want 1", st.WrongVersion)
	}
	if st.BadFrames != uint64(len(inputs)) {
		t.Errorf("bad_frames=%d, want %d", st.BadFrames, len(inputs))
	}
	if got := w.Rows(); got != 0 {
		t.Errorf("garbage produced %d rows", got)
	}
}

// A HELLO whose contents do not hash to its claimed config_id means the sender's hash
// disagrees with ours. Catching it here is what turns a silent total failure — every
// DATA frame rejected as unknown — into one clear warning.
func TestHelloWithMismatchedConfigIDIsRejected(t *testing.T) {
	r, w := newTestRegistry(t, Config{})
	hello := loadFixture(t, "hello")
	binary.LittleEndian.PutUint64(hello[16:24], 0xdeadbeefdeadbeef)

	r.Handle(hello, time.Now())

	st := r.Stats()
	if st.Configs != 0 {
		t.Errorf("configs=%d, want 0 — a mismatched config_id must not register", st.Configs)
	}
	if st.BadFrames != 1 {
		t.Errorf("bad_frames=%d, want 1", st.BadFrames)
	}
	if got := w.Rows(); got != 0 {
		t.Errorf("wrote %d rows", got)
	}
}
