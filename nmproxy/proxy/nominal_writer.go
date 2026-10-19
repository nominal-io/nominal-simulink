package proxy

import (
	"context"
	"fmt"
	"log/slog"
	"sync"

	nominal "github.com/nominal-io/nominal-streaming-go"
)

// NominalWriter writes to Nominal through the official Go client.
//
// The proxy exists so a constrained host does not have to do this work, so it is worth
// being clear about what "this work" is: protobuf encoding, TLS, HTTP, batching and
// retries — all of it here rather than on the machine running the model.
//
// Deliberately built on nominal-streaming-go rather than reimplementing the write path.
// The frame format between host and relay is ours and had to be written three times. The
// Nominal protocol is not, and one maintained implementation is better than a second
// hand-rolled one.
type NominalWriter struct {
	client *nominal.Client
	log    *slog.Logger

	mu      sync.Mutex
	streams []*nominalStream
}

// NewNominalWriter writes with the API key and base URL in cfg.Nominal, the profile
// LoadConfig read.
func NewNominalWriter(cfg Config, log *slog.Logger) (*NominalWriter, error) {
	client, err := nominal.NewClient(cfg.Nominal.Token, nominal.WithBaseURL(cfg.Nominal.BaseURL))
	if err != nil {
		return nil, fmt.Errorf("creating the Nominal client: %w", err)
	}
	return &NominalWriter{client: client, log: log}, nil
}

func (w *NominalWriter) Open(datasetRID string, channels []string, tags map[string]string,
	onError func(error)) (Stream, error) {
	ds, errCh, err := w.client.NewDatasetStream(context.Background(), datasetRID)
	if err != nil {
		return nil, fmt.Errorf("opening a stream for %s: %w", datasetRID, err)
	}

	s := &nominalStream{
		rid:      datasetRID,
		ds:       ds,
		channels: make([]*nominal.ChannelStream[float64], len(channels)),
		log:      w.log,
		onError:  onError,
		drained:  make(chan struct{}),
	}

	// One typed stream per channel, resolved once. Frames carry channel *indices*, so
	// doing this up front means the receive path never looks a name up.
	for i, name := range channels {
		s.channels[i] = ds.FloatStream(name, nominal.WithTags(tags))
	}

	// The client reports write failures on errCh and drops the oldest when it is full.
	// Draining errCh here is the only way those failures reach write_errors and the log.
	// The client closes errCh at the end of Close, which ends this goroutine.
	go func() {
		defer close(s.drained)
		for err := range errCh {
			s.recordError(err)
		}
	}()

	w.mu.Lock()
	w.streams = append(w.streams, s)
	w.mu.Unlock()

	w.log.Info("stream opened", "dataset", datasetRID, "channels", len(channels), "tags", tags)
	return s, nil
}

// Close releases the client. Streams are closed individually by the registry, which
// owns their lifetime.
func (w *NominalWriter) Close() error {
	return w.client.Close()
}

type nominalStream struct {
	rid      string
	ds       *nominal.DatasetStream
	channels []*nominal.ChannelStream[float64]
	log      *slog.Logger
	onError  func(error)
	drained  chan struct{} // closed once every error the client reported is recorded

	mu       sync.Mutex
	errCount uint64
	lastErr  error
}

func (s *nominalStream) Push(timestampNanos int64, values []float64) error {
	if len(values) != len(s.channels) {
		return fmt.Errorf("row has %d values, stream has %d channels",
			len(values), len(s.channels))
	}
	// By index, never by name: the frame already told us the order at HELLO.
	for i, v := range values {
		s.channels[i].Enqueue(timestampNanos, v)
	}
	return nil
}

func (s *nominalStream) Close() error {
	// Blocks until the final batch has been sent, which is what makes an evicted or
	// shut-down stream still send its data.
	err := s.ds.Close()

	// Errors the client reported before Close can still be waiting in errCh. Wait for
	// the drain so they are counted before choosing which line to log.
	//
	// The client discards errors from batches that finish after Close starts, including
	// the final one, so a failure of that batch still goes unreported here and the line
	// below can say "stream closed". nominal-streaming-go has to deliver those errors on
	// errCh before closing it for nmproxy to count them.
	<-s.drained

	s.mu.Lock()
	n, last := s.errCount, s.lastErr
	s.mu.Unlock()

	if n > 0 {
		s.log.Warn("stream closed with errors", "dataset", s.rid,
			"errors", n, "last", last)
	} else {
		s.log.Info("stream closed", "dataset", s.rid)
	}
	return err
}

func (s *nominalStream) recordError(err error) {
	s.mu.Lock()
	s.errCount++
	n := s.errCount
	s.lastErr = err
	s.mu.Unlock()

	// Push only enqueues, so this is the only place a failed write to Nominal surfaces.
	// Without passing it on, a bad token would leave write_errors at zero.
	if s.onError != nil {
		s.onError(err)
	}

	// Rate-limited: a broken token or a wrong base URL fails every request, and logging
	// each one buries whatever else is happening.
	if n == 1 || n%100 == 0 {
		s.log.Error("write failed", "dataset", s.rid, "error", err, "count", n)
	}
}
