package proxy

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"time"

	"github.com/nominal-io/nominal-simulink/nmproxy/frame"
)

// Server receives frames and hands them to a Registry.
type Server struct {
	cfg      Config
	registry *Registry
	log      *slog.Logger
	conn     *net.UDPConn
}

func NewServer(cfg Config, registry *Registry, log *slog.Logger) (*Server, error) {
	addr, err := net.ResolveUDPAddr("udp", cfg.Listen)
	if err != nil {
		return nil, fmt.Errorf("resolving %q: %w", cfg.Listen, err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return nil, fmt.Errorf("listening on %q: %w", cfg.Listen, err)
	}

	// Generous receive buffer. The kernel drops datagrams silently once this fills,
	// and those drops are invisible to us — they do not even appear as sequence gaps
	// until the next frame arrives. Buying headroom here is much cheaper than
	// diagnosing loss later.
	if err := conn.SetReadBuffer(8 << 20); err != nil {
		log.Warn("could not enlarge the receive buffer; bursts may be dropped by the kernel",
			"error", err)
	}

	return &Server{cfg: cfg, registry: registry, log: log, conn: conn}, nil
}

func (s *Server) LocalAddr() net.Addr { return s.conn.LocalAddr() }

// Run receives until the context is cancelled.
//
// Single-threaded on purpose. One goroutine reading a socket keeps frames from a session
// in arrival order, which matters because sequence gap detection assumes it; a worker
// pool would manufacture reordering that never happened on the wire. If one core ever
// becomes the bottleneck, the fix is SO_REUSEPORT with a socket per worker, which
// preserves per-flow ordering — not a shared queue.
func (s *Server) Run(ctx context.Context) error {
	// Room for any UDP datagram, not only the largest frame. Anyone who can reach the
	// port can send anything, and on Windows a datagram larger than the buffer fails
	// the read itself rather than arriving truncated.
	buf := make([]byte, 64<<10)

	go func() {
		<-ctx.Done()
		// Unblocks ReadFromUDP; there is no other way to interrupt it.
		_ = s.conn.Close()
	}()

	if s.cfg.StatsInterval > 0 {
		go s.reportStats(ctx)
	}
	go s.sweep(ctx)

	// One unreadable datagram is no reason to stop serving every other sender. A socket
	// that fails every read is, so a long enough run of failures still ends the relay.
	const maxConsecutiveFailures = 100
	failures := 0

	for {
		n, _, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil {
				return nil // closed by the cancellation above
			}
			if errors.Is(err, net.ErrClosed) {
				return fmt.Errorf("receive failed: %w", err)
			}
			failures++
			if failures >= maxConsecutiveFailures {
				return fmt.Errorf("receive failed %d times in a row: %w", failures, err)
			}
			s.registry.countBadFrame()
			if failures == 1 {
				s.log.Warn("a datagram could not be read", "error", err)
			}
			continue
		}
		failures = 0

		// No sender of ours exceeds MaxBytes, so this is another protocol or rubbish.
		if n > frame.MaxBytes {
			s.registry.countBadFrame()
			continue
		}
		s.registry.Handle(buf[:n], time.Now())
	}
}

func (s *Server) reportStats(ctx context.Context) {
	t := time.NewTicker(s.cfg.StatsInterval)
	defer t.Stop()

	var prev Stats
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			st := s.registry.Stats()
			// Deltas rather than totals: a running daemon's absolute counters stop
			// being informative after a day, whereas "rows since last line" tells you
			// whether data is flowing right now.
			s.log.Info("stats",
				"configs", st.Configs,
				"sessions", st.Sessions,
				"rows", st.Rows-prev.Rows,
				"gaps", st.Gaps-prev.Gaps,
				"reorders", st.Reorders-prev.Reorders,
				"unknown_config", st.UnknownConfig-prev.UnknownConfig,
				"bad_frames", st.BadFrames-prev.BadFrames,
				"wrong_version", st.WrongVersion-prev.WrongVersion,
				"write_errors", st.WriteErrors-prev.WriteErrors,
			)
			prev = st
		}
	}
}

func (s *Server) sweep(ctx context.Context) {
	// Sweeping far more often than the timeout would waste wakeups; far less often
	// would let evicted streams sit unflushed. A quarter of the timeout is a
	// reasonable compromise.
	interval := s.cfg.IdleTimeout / 4
	if interval < time.Second {
		interval = time.Second
	}
	t := time.NewTicker(interval)
	defer t.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.registry.Sweep(time.Now())
		}
	}
}

// Close releases the socket and flushes every open stream.
func (s *Server) Close() {
	_ = s.conn.Close()
	s.registry.Close()
}
