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
// The frame format between host and relay is ours and had to be written three times; the
// Nominal protocol is not, and one maintained implementation is better than a second
// hand-rolled one.
type NominalWriter struct {
	client *nominal.Client
	log    *slog.Logger

	mu      sync.Mutex
	streams []*nominalStream
}

func NewNominalWriter(cfg Config, log *slog.Logger) (*NominalWriter, error) {
	opts := []nominal.Option{}
	if cfg.Nominal.BaseURL != "" {
		// Worth passing explicitly: a RID from one environment does not resolve
		// against another, and the default is production.
		opts = append(opts, nominal.WithBaseURL(cfg.Nominal.BaseURL))
	}

	client, err := nominal.NewClient(cfg.Nominal.Token, opts...)
	if err != nil {
		return nil, fmt.Errorf("creating the Nominal client: %w", err)
	}
	return &NominalWriter{client: client, log: log}, nil
}

func (w *NominalWriter) Open(datasetRID string, channels []string, onError func(error)) (Stream, error) {
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
	}

	// One typed stream per channel, resolved once. Frames carry channel *indices*, so
	// doing this up front means the receive path never looks a name up.
	for i, name := range channels {
		s.channels[i] = ds.FloatStream(name)
	}

	// The client reports asynchronously. Without draining this the channel fills and
	// the client blocks, so a failure to write would eventually become a failure to
	// receive — the proxy would go quiet rather than complain.
	go func() {
		for err := range errCh {
			s.recordError(err)
		}
	}()

	w.mu.Lock()
	w.streams = append(w.streams, s)
	w.mu.Unlock()

	w.log.Info("stream opened", "dataset", datasetRID, "channels", len(channels))
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
	// Blocks until everything queued has been sent, which is what makes an evicted or
	// shut-down stream still land its data.
	err := s.ds.Close()

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
