package proxy

import (
	"fmt"
	"log/slog"
	"sync"
)

// Writer opens a stream for one dataset and channel set.
//
// Deliberately mirrors the C library's nm_open / nm_push / nm_close, because the proxy
// is doing the same job the host would have done in direct mode — it has simply moved
// off the constrained machine. Keeping the shapes aligned means the two paths behave the
// same way under backpressure and failure.
//
// An interface rather than a concrete type so the session and gap logic is testable
// without a token or a network, and so the real writer can be swapped without touching
// anything above it.
type Writer interface {
	// Open starts a stream. onError is called for every write that fails after Push has
	// returned: a writer that batches and sends in the background only learns of a
	// failure later, and the caller has to count those failures all the same. It may be
	// called from any goroutine.
	Open(datasetRID string, channels []string, onError func(error)) (Stream, error)
}

// Stream accepts rows for a fixed channel set, in the order Open was given them.
type Stream interface {
	// Push writes one row. Implementations must not block indefinitely: the caller is
	// the UDP receive loop, and stalling it drops datagrams the kernel cannot buffer.
	Push(timestampNanos int64, values []float64) error
	Close() error
}

// LogWriter records what it was asked to write and does nothing else.
//
// Not only a test double — it is genuinely useful for diagnosing a deployment, since it
// answers "are frames arriving, and do they decode into sensible values?" without a
// token or any network egress. `nmproxy -dry-run` uses it.
type LogWriter struct {
	Log     *slog.Logger
	Verbose bool

	mu      sync.Mutex
	streams []*LogStream
}

// Open never reports a failure through onError: nothing is sent, so nothing can fail
// later.
func (w *LogWriter) Open(datasetRID string, channels []string, _ func(error)) (Stream, error) {
	s := &LogStream{rid: datasetRID, channels: channels, parent: w}

	w.mu.Lock()
	w.streams = append(w.streams, s)
	w.mu.Unlock()

	w.Log.Info("stream opened (dry run: nothing will be written)",
		"dataset", datasetRID, "channels", len(channels))
	return s, nil
}

// Rows reports what every stream received, for assertions in tests.
func (w *LogWriter) Rows() int {
	w.mu.Lock()
	defer w.mu.Unlock()

	total := 0
	for _, s := range w.streams {
		total += s.Rows()
	}
	return total
}

type LogStream struct {
	rid      string
	channels []string
	parent   *LogWriter

	mu   sync.Mutex
	rows int
	last []float64
}

func (s *LogStream) Push(timestampNanos int64, values []float64) error {
	if len(values) != len(s.channels) {
		return fmt.Errorf("row has %d values, stream has %d channels",
			len(values), len(s.channels))
	}

	s.mu.Lock()
	s.rows++
	s.last = append(s.last[:0], values...)
	n := s.rows
	s.mu.Unlock()

	if s.parent.Verbose {
		s.parent.Log.Debug("row", "dataset", s.rid, "ts", timestampNanos,
			"values", values, "total", n)
	}
	return nil
}

func (s *LogStream) Close() error {
	s.parent.Log.Info("stream closed", "dataset", s.rid, "rows", s.Rows())
	return nil
}

func (s *LogStream) Rows() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.rows
}
