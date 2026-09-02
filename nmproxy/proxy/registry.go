package proxy

import (
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/nominal-io/nominal-simulink/nmproxy/frame"
)

// The two ids in a frame answer different questions, so they are tracked separately.
//
//	config_id  identifies a *configuration* — a dataset and an ordered channel list.
//	           It is what a DATA frame is resolved through, and what a stream is opened
//	           for. Two runs of the same model share one.
//	session_id identifies a *run*. Sequence numbers are per-session, so gap detection
//	           has to be too; two models streaming at once would otherwise look like one
//	           session with wildly out-of-order sequences.
//
// Conflating them would mean either losing gap detection when a model restarts, or
// opening a redundant stream per run.

type config struct {
	id       uint64
	rid      string
	channels []string
	stream   Stream
	lastSeen time.Time
}

type session struct {
	id       uint64
	configID uint64

	// Sequence tracking. The host advances the sequence even when a send fails, so a
	// gap genuinely means data did not arrive rather than merely was not sent.
	started  bool
	nextSeq  uint64
	gaps     uint64 // frames the sequence says we never received
	reorders uint64 // arrived after a later frame; UDP is allowed to do this

	rows     uint64
	lastSeen time.Time
}

// Registry maps frames onto open streams, and is the only mutable state the proxy keeps.
type Registry struct {
	writer Writer
	log    *slog.Logger
	cfg    Config

	// How long a configuration or session may go unheard before it is evicted. Without
	// this a long-lived daemon accumulates a stream per model run forever.
	idleTimeout time.Duration

	mu       sync.Mutex
	configs  map[uint64]*config
	sessions map[uint64]*session

	// Counters for frames that never reach a stream. Reported rather than silently
	// discarded, because "no data arrived" needs to distinguish between nothing being
	// sent, frames being rejected, and writes failing.
	unknownConfig uint64
	badFrames     uint64
	wrongVersion  uint64
	writeErrors   uint64
	refused       uint64 // datasets not on the allowlist
}

func NewRegistry(w Writer, cfg Config, log *slog.Logger) *Registry {
	idle := cfg.IdleTimeout
	if idle <= 0 {
		idle = 5 * time.Minute
	}
	return &Registry{
		writer:      w,
		cfg:         cfg,
		log:         log,
		idleTimeout: idle,
		configs:     map[uint64]*config{},
		sessions:    map[uint64]*session{},
	}
}

// Handle processes one datagram. Returns an error only for genuinely unexpected
// problems; malformed input is counted and swallowed, since an unauthenticated sender
// on the network must not be able to stop the proxy by sending rubbish.
func (r *Registry) Handle(buf []byte, now time.Time) {
	h, err := frame.DecodeHeader(buf)
	if err != nil {
		r.mu.Lock()
		if errors.Is(err, frame.ErrWrongVersion) {
			r.wrongVersion++
			// Logged, not just counted: there is no back-channel to tell the host it is
			// speaking a version we do not, so without this a mismatch is
			// indistinguishable from a network fault.
			if r.wrongVersion == 1 || r.wrongVersion%1000 == 0 {
				r.log.Warn("rejecting frames with an unsupported protocol version",
					"error", err, "count", r.wrongVersion)
			}
		} else {
			r.badFrames++
		}
		r.mu.Unlock()
		return
	}

	switch h.Type {
	case frame.TypeHello:
		r.handleHello(buf, now)
	case frame.TypeData:
		r.handleData(buf, now)
	}
}

func (r *Registry) handleHello(buf []byte, now time.Time) {
	hello, err := frame.DecodeHello(buf)
	if err != nil {
		r.mu.Lock()
		r.badFrames++
		r.mu.Unlock()
		return
	}

	// A HELLO is only trustworthy if its contents hash to the config_id it claims.
	// Cheap to check and it catches a mismatched implementation immediately, rather
	// than letting DATA frames pile up against a config that can never resolve.
	if want := frame.ConfigID(hello.DatasetRID, hello.Channels); want != hello.ConfigID {
		r.mu.Lock()
		r.badFrames++
		r.mu.Unlock()
		r.log.Warn("HELLO config_id does not match its contents — the sender's hash disagrees with ours",
			"claimed", hello.ConfigID, "computed", want, "dataset", hello.DatasetRID)
		return
	}

	// Checked here rather than per-DATA-frame: a refused dataset never gets a config
	// registered, so its data frames fall through to the unknown-config path and cost
	// nothing more than a counter.
	if !r.cfg.DatasetAllowed(hello.DatasetRID) {
		r.mu.Lock()
		r.refused++
		first := r.refused == 1
		r.mu.Unlock()
		if first {
			r.log.Warn("refusing a dataset that is not in allowed_datasets",
				"dataset", hello.DatasetRID)
		}
		return
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	if c, ok := r.configs[hello.ConfigID]; ok {
		// The expected case: HELLO repeats about once a second, so almost every one
		// is a refresh of something already known.
		c.lastSeen = now
		return
	}

	stream, err := r.writer.Open(hello.DatasetRID, hello.Channels)
	if err != nil {
		r.writeErrors++
		r.log.Error("could not open a stream for a new configuration",
			"dataset", hello.DatasetRID, "error", err)
		return
	}

	r.configs[hello.ConfigID] = &config{
		id:       hello.ConfigID,
		rid:      hello.DatasetRID,
		channels: hello.Channels,
		stream:   stream,
		lastSeen: now,
	}
	r.log.Info("configuration registered",
		"config", hello.ConfigID, "dataset", hello.DatasetRID,
		"channels", len(hello.Channels))
}

func (r *Registry) handleData(buf []byte, now time.Time) {
	data, err := frame.DecodeData(buf)
	if err != nil {
		r.mu.Lock()
		r.badFrames++
		r.mu.Unlock()
		return
	}

	r.mu.Lock()

	c, known := r.configs[data.ConfigID]
	if !known {
		// Expected after a proxy restart, and bounded: the host repeats HELLO about
		// once a second, so this resolves itself.
		r.unknownConfig++
		if r.unknownConfig == 1 {
			r.log.Info("data for an unknown configuration — waiting for a HELLO",
				"config", data.ConfigID)
		}
		r.mu.Unlock()
		return
	}

	// A DATA frame whose width disagrees with its HELLO would write values into the
	// wrong channels. Refuse rather than guess.
	if data.ChannelCount != len(c.channels) {
		r.badFrames++
		r.mu.Unlock()
		r.log.Warn("data width does not match the registered configuration",
			"config", data.ConfigID, "frame", data.ChannelCount, "expected", len(c.channels))
		return
	}

	s, ok := r.sessions[data.SessionID]
	if !ok {
		s = &session{id: data.SessionID, configID: data.ConfigID}
		r.sessions[data.SessionID] = s
		r.log.Info("session started", "session", data.SessionID, "config", data.ConfigID)
	}
	r.trackSequence(s, data.Sequence)

	c.lastSeen = now
	s.lastSeen = now
	s.rows += uint64(len(data.Rows))
	stream := c.stream

	r.mu.Unlock()

	// Written outside the lock: a slow write must not block other sessions, and the
	// receive loop is what suffers if this takes too long.
	for _, row := range data.Rows {
		if err := stream.Push(row.TimestampNanos, row.Values); err != nil {
			r.mu.Lock()
			r.writeErrors++
			n := r.writeErrors
			r.mu.Unlock()
			if n == 1 || n%1000 == 0 {
				r.log.Error("write failed", "config", data.ConfigID, "error", err, "count", n)
			}
			return
		}
	}
}

// trackSequence must be called with the lock held.
func (r *Registry) trackSequence(s *session, seq uint64) {
	switch {
	case !s.started:
		s.started = true
		s.nextSeq = seq + 1

	case seq == s.nextSeq:
		s.nextSeq++

	case seq > s.nextSeq:
		// Frames between nextSeq and seq never arrived. Counting the difference rather
		// than incrementing by one keeps the number meaningful when a burst is lost.
		s.gaps += seq - s.nextSeq
		s.nextSeq = seq + 1

	default:
		// Arrived after a frame with a higher sequence. UDP is permitted to reorder, so
		// this is not loss and must not be counted as such — a reordered frame was
		// already counted as a gap when the later one arrived.
		s.reorders++
	}
}

// Stats is a snapshot for logging or a metrics endpoint.
type Stats struct {
	Configs       int
	Sessions      int
	Rows          uint64
	Gaps          uint64
	Reorders      uint64
	UnknownConfig uint64
	BadFrames     uint64
	WrongVersion  uint64
	WriteErrors   uint64
	Refused       uint64
}

func (r *Registry) Stats() Stats {
	r.mu.Lock()
	defer r.mu.Unlock()

	st := Stats{
		Configs:       len(r.configs),
		Sessions:      len(r.sessions),
		UnknownConfig: r.unknownConfig,
		BadFrames:     r.badFrames,
		WrongVersion:  r.wrongVersion,
		WriteErrors:   r.writeErrors,
		Refused:       r.refused,
	}
	for _, s := range r.sessions {
		st.Rows += s.rows
		st.Gaps += s.gaps
		st.Reorders += s.reorders
	}
	return st
}

// Sweep evicts configurations and sessions that have gone quiet, closing their streams.
// Closing is what flushes buffered rows, so an evicted stream's data still lands.
func (r *Registry) Sweep(now time.Time) {
	r.mu.Lock()

	var closing []Stream
	for id, s := range r.sessions {
		if now.Sub(s.lastSeen) > r.idleTimeout {
			r.log.Info("session idle, forgetting it",
				"session", id, "rows", s.rows, "gaps", s.gaps, "reorders", s.reorders)
			delete(r.sessions, id)
		}
	}
	for id, c := range r.configs {
		if now.Sub(c.lastSeen) > r.idleTimeout {
			r.log.Info("configuration idle, closing its stream",
				"config", id, "dataset", c.rid)
			closing = append(closing, c.stream)
			delete(r.configs, id)
		}
	}

	r.mu.Unlock()

	for _, s := range closing {
		if err := s.Close(); err != nil {
			r.log.Error("closing an idle stream failed", "error", err)
		}
	}
}

// Close flushes and releases everything. Streams are closed so buffered rows land.
func (r *Registry) Close() {
	r.mu.Lock()
	streams := make([]Stream, 0, len(r.configs))
	for _, c := range r.configs {
		streams = append(streams, c.stream)
	}
	r.configs = map[uint64]*config{}
	r.sessions = map[uint64]*session{}
	r.mu.Unlock()

	for _, s := range streams {
		if err := s.Close(); err != nil {
			r.log.Error("closing a stream failed", "error", err)
		}
	}
}
