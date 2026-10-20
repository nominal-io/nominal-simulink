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
//
// One stream per dataset, shared by every channel list that writes to it. The client
// gives each DatasetStream its own HTTP client and batching goroutine, and makes its TLS
// connection on its first write, so a stream per channel list would pay for a connection
// on every execution: the execution ID is a tag, and a new tag is a new channel list.
// Shared, an execution costs a few map entries, and a campaign of short executions keeps
// one connection warm. The stream closes once no channel list uses it.
//
// The client never forgets a (channel, tags) pair it has seen, and its batcher walks
// every pair on each flush, so a stream that outlives many executions would grow without
// bound. Past maxChannelStreams pairs the writer opens a fresh stream for the dataset,
// moves the channel lists still open onto it, and closes the old one in the background,
// which flushes it. That costs one connection every maxChannelStreams/channels
// executions instead of one per execution.
type NominalWriter struct {
	client *nominal.Client
	log    *slog.Logger

	// How many (channel, tags) pairs a dataset's stream may hold before it is replaced.
	maxChannelStreams int

	mu       sync.Mutex
	datasets map[string]*datasetStream // by RID, while a channel list uses it
	closing  sync.WaitGroup            // streams being closed in the background
}

const defaultMaxChannelStreams = 16384

// NewNominalWriter writes with the API key and base URL in cfg.Nominal, the profile
// LoadConfig read.
func NewNominalWriter(cfg Config, log *slog.Logger) (*NominalWriter, error) {
	client, err := nominal.NewClient(cfg.Nominal.Token, nominal.WithBaseURL(cfg.Nominal.BaseURL))
	if err != nil {
		return nil, fmt.Errorf("creating the Nominal client: %w", err)
	}
	return &NominalWriter{
		client:            client,
		log:               log,
		maxChannelStreams: defaultMaxChannelStreams,
		datasets:          map[string]*datasetStream{},
	}, nil
}

// Open binds a channel list onto the dataset's stream, starting the stream if the
// dataset has none. The dataset reports its write failures through the onError of the
// Open that started its stream; the registry passes the same function every time.
func (w *NominalWriter) Open(datasetRID string, channels []string, tags map[string]string,
	onError func(error)) (Stream, error) {
	w.mu.Lock()
	defer w.mu.Unlock()

	d, ok := w.datasets[datasetRID]
	if !ok {
		d = &datasetStream{
			rid:     datasetRID,
			log:     w.log,
			onError: onError,
			users:   map[*nominalStream]struct{}{},
		}
		if err := w.start(d); err != nil {
			return nil, fmt.Errorf("opening a stream for %s: %w", datasetRID, err)
		}
		w.datasets[datasetRID] = d
		w.log.Info("stream opened", "dataset", datasetRID)
	} else if d.pairs+len(channels) > w.maxChannelStreams {
		if err := w.reopen(d); err != nil {
			return nil, fmt.Errorf("reopening the stream for %s: %w", datasetRID, err)
		}
	}

	s := &nominalStream{w: w, dataset: d, names: channels, tags: tags}
	s.channels = d.bind(channels, tags)
	d.users[s] = struct{}{}
	return s, nil
}

// start gives d a new client stream. It must be called with the lock held.
func (w *NominalWriter) start(d *datasetStream) error {
	ds, errCh, err := w.client.NewDatasetStream(context.Background(), d.rid)
	if err != nil {
		return err
	}
	d.ds = ds
	d.pairs = 0
	d.drained = make(chan struct{})

	// The client reports write failures on errCh and drops the oldest when it is full.
	// Draining errCh here is the only way those failures reach write_errors and the log.
	// The client closes errCh at the end of Close, which ends this goroutine.
	drained := d.drained
	go func() {
		defer close(drained)
		for err := range errCh {
			d.recordError(err)
		}
	}()
	return nil
}

// reopen replaces d's client stream with a fresh one, moving every channel list still
// open onto it, and closes the old one in the background. It must be called with the
// lock held.
func (w *NominalWriter) reopen(d *datasetStream) error {
	old, oldDrained, pairs := d.ds, d.drained, d.pairs
	if err := w.start(d); err != nil {
		d.ds, d.drained, d.pairs = old, oldDrained, pairs
		return err
	}
	for u := range d.users {
		u.mu.Lock()
		u.channels = d.bind(u.names, u.tags)
		u.mu.Unlock()
	}
	w.log.Info("stream reopened", "dataset", d.rid, "channel_streams", pairs)

	w.closing.Add(1)
	go func() {
		defer w.closing.Done()
		if err := d.finish(old, oldDrained, false); err != nil {
			w.log.Error("closing a replaced stream failed", "dataset", d.rid, "error", err)
		}
	}()
	return nil
}

// Close waits for streams still closing in the background, closes any left open, and
// releases the client. Channel lists are closed individually by the registry, which owns
// their lifetime, so by the time it gets here every stream is normally closed already.
func (w *NominalWriter) Close() error {
	w.mu.Lock()
	left := make([]*datasetStream, 0, len(w.datasets))
	for _, d := range w.datasets {
		left = append(left, d)
	}
	w.datasets = map[string]*datasetStream{}
	w.mu.Unlock()

	var first error
	for _, d := range left {
		if err := d.finish(d.ds, d.drained, true); err != nil && first == nil {
			first = err
		}
	}
	w.closing.Wait()
	if err := w.client.Close(); err != nil && first == nil {
		first = err
	}
	return first
}

// datasetStream is the client's stream to one dataset and the channel lists on it. ds,
// drained, pairs and users belong to NominalWriter.mu; the error fields to mu.
type datasetStream struct {
	rid     string
	log     *slog.Logger
	onError func(error)

	ds      *nominal.DatasetStream
	drained chan struct{} // closed once every error ds reported is recorded
	pairs   int           // (channel, tags) pairs bound onto ds
	users   map[*nominalStream]struct{}

	mu       sync.Mutex
	errCount uint64
	lastErr  error
}

// bind resolves one typed stream per channel, once. Frames carry channel *indices*, so
// doing this up front means the receive path never looks a name up. It must be called
// with the writer's lock held.
func (d *datasetStream) bind(names []string, tags map[string]string) []*nominal.ChannelStream[float64] {
	channels := make([]*nominal.ChannelStream[float64], len(names))
	for i, name := range names {
		channels[i] = d.ds.FloatStream(name, nominal.WithTags(tags))
	}
	d.pairs += len(names)
	return channels
}

// finish closes a client stream, which blocks until its final batch has been sent, and
// is what makes an evicted or shut-down stream still send its data. With announce set,
// it logs the close.
func (d *datasetStream) finish(ds *nominal.DatasetStream, drained chan struct{}, announce bool) error {
	err := ds.Close()

	// Errors the client reported before Close can still be waiting in errCh. Wait for
	// the drain so they are counted before choosing which line to log.
	//
	// The client discards errors from batches that finish after Close starts, including
	// the final one, so a failure of that batch still goes unreported here and the line
	// below can say "stream closed". nominal-streaming-go has to deliver those errors on
	// errCh before closing it for nmproxy to count them.
	<-drained

	if !announce {
		return err
	}
	d.mu.Lock()
	n, last := d.errCount, d.lastErr
	d.mu.Unlock()

	if n > 0 {
		d.log.Warn("stream closed with errors", "dataset", d.rid,
			"errors", n, "last", last)
	} else {
		d.log.Info("stream closed", "dataset", d.rid)
	}
	return err
}

func (d *datasetStream) recordError(err error) {
	d.mu.Lock()
	d.errCount++
	n := d.errCount
	d.lastErr = err
	d.mu.Unlock()

	// Push only enqueues, so this is the only place a failed write to Nominal surfaces.
	// Without passing it on, a bad token would leave write_errors at zero.
	if d.onError != nil {
		d.onError(err)
	}

	// Rate-limited: a broken token or a wrong base URL fails every request, and logging
	// each one buries whatever else is happening.
	if n == 1 || n%100 == 0 {
		d.log.Error("write failed", "dataset", d.rid, "error", err, "count", n)
	}
}

// nominalStream is one channel list on a dataset's stream.
type nominalStream struct {
	w       *NominalWriter
	dataset *datasetStream
	names   []string
	tags    map[string]string

	mu       sync.RWMutex
	channels []*nominal.ChannelStream[float64] // nil once closed
}

func (s *nominalStream) Push(timestampNanos int64, values []float64) error {
	s.mu.RLock()
	defer s.mu.RUnlock()

	// A row for a channel list the registry has just forgotten has nowhere to go, and
	// isn't a failure worth a log line.
	if s.channels == nil {
		return nil
	}
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

// Close takes the channel list off its dataset's stream, and closes the stream when it
// was the last one on it.
func (s *nominalStream) Close() error {
	s.w.mu.Lock()

	s.mu.Lock()
	s.channels = nil
	s.mu.Unlock()

	d := s.dataset
	delete(d.users, s)
	if len(d.users) > 0 {
		s.w.mu.Unlock()
		return nil
	}
	delete(s.w.datasets, d.rid)
	ds, drained := d.ds, d.drained
	s.w.mu.Unlock()

	return d.finish(ds, drained, true)
}
