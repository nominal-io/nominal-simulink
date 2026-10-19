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
//	session_id identifies a *run*. Sequence numbers are per session, so gap detection
//	           is too. Tracked per configuration, two runs of one model streaming at
//	           once would look like one session with wildly out-of-order sequences.
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
	// gap counts every frame that did not arrive, whether the network lost it or the
	// host could not send it.
	started  bool
	nextSeq  uint64
	gaps     uint64 // frames the sequence says we never received
	reorders uint64 // arrived after a later frame, or a duplicate

	rows     uint64
	lastSeen time.Time
}

// Registry maps frames onto open streams and holds the proxy's configuration and session
// state.
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

	// Counters for frames that never reach a stream, and for writes that fail. Reported
	// rather than silently discarded, because "no data arrived" needs to distinguish
	// between nothing being sent, frames being rejected, and writes failing.
	unknownConfig uint64
	badFrames     uint64
	wrongVersion  uint64
	writeErrors   uint64
	refused       uint64 // HELLOs for datasets not on the allowlist

	// RIDs already warned about, so each refused dataset is logged once rather than once
	// a second. Capped at maxRefusedRIDsLogged, because any sender can invent RIDs and
	// neither the log nor this set should grow with them.
	refusedRIDs map[string]struct{}

	// What sessions counted before they were forgotten. Stats adds these to the live
	// sessions' counts so the totals only ever grow; without them a total falls when a
	// session is evicted, and the periodic delta wraps to around 1.8e19.
	retiredRows     uint64
	retiredGaps     uint64
	retiredReorders uint64
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
		refusedRIDs: map[string]struct{}{},
	}
}

// maxRefusedRIDsLogged bounds how many distinct refused datasets are named in the log.
const maxRefusedRIDsLogged = 100

// Handle processes one datagram. Malformed input is counted and dropped, so an
// unauthenticated sender cannot stop the proxy by sending rubbish.
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
				r.log.Warn("rejecting frames that use another frame protocol version. "+
					"Use an nmproxy release that speaks the sender's version (see nmproxy -version).",
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
	if want := frame.ConfigID(hello.DatasetRID, hello.Channels, hello.Tags); want != hello.ConfigID {
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
		_, seen := r.refusedRIDs[hello.DatasetRID]
		first := !seen && len(r.refusedRIDs) < maxRefusedRIDsLogged
		if first {
			r.refusedRIDs[hello.DatasetRID] = struct{}{}
		}
		r.mu.Unlock()
		if first {
			r.log.Warn("refusing a dataset that is not in allowed_datasets. "+
				"Add its RID to allowed_datasets if nmproxy should write to it.",
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

	tags := make(map[string]string, len(hello.Tags))
	for _, tag := range hello.Tags {
		tags[tag.Key] = tag.Value
	}
	stream, err := r.writer.Open(hello.DatasetRID, hello.Channels, tags, r.countWriteError)
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
		// Expected for up to a second after a proxy restart, until the host's next
		// HELLO. DATA for a dataset refused at HELLO also lands here, and that does not
		// resolve.
		r.unknownConfig++
		if r.unknownConfig == 1 {
			r.log.Info("dropping data for a configuration with no accepted HELLO",
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

	// Pushed outside the lock so the stats reporter, the sweeper and the writer's error
	// callback are not held up. Push runs on the receive loop, so a slow Push still
	// delays every session.
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

// countBadFrame records a datagram that never reached the decoder, such as one too
// large to be a frame.
func (r *Registry) countBadFrame() {
	r.mu.Lock()
	r.badFrames++
	r.mu.Unlock()
}

// countWriteError records a write that failed after Push returned. The writer logs the
// error itself; this only has to make it visible in the stats.
func (r *Registry) countWriteError(error) {
	r.mu.Lock()
	r.writeErrors++
	r.mu.Unlock()
}

// retire folds a session's counts into the totals before it is forgotten. It must be
// called with the lock held.
func (r *Registry) retire(s *session) {
	r.retiredRows += s.rows
	r.retiredGaps += s.gaps
	r.retiredReorders += s.reorders
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
		// Arrived after a frame with a higher sequence, or is a duplicate. Its gap was
		// counted when the later frame arrived and stays counted, so this counts only
		// as a reorder.
		s.reorders++
	}
}

// Stats is a snapshot for logging or a metrics endpoint.
//
// Configs and Sessions are what is live now. Every other field is a total since the
// registry started, including sessions since forgotten, so it never decreases and the
// difference between two snapshots is always what happened in between.
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
		Rows:          r.retiredRows,
		Gaps:          r.retiredGaps,
		Reorders:      r.retiredReorders,
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
			r.retire(s)
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
	// Retired rather than dropped, so the totals logged at shutdown still include them.
	for _, s := range r.sessions {
		r.retire(s)
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
