package proxy

import (
	"context"
	"net"
	"testing"
	"time"
)

// startServer runs a relay on an ephemeral loopback port and returns where to send.
func startServer(t *testing.T) (*Registry, *net.UDPAddr, <-chan error) {
	t.Helper()
	cfg := DefaultConfig()
	cfg.Listen = "127.0.0.1:0"
	cfg.StatsInterval = 0
	r := NewRegistry(&LogWriter{Log: quietLogger()}, cfg, quietLogger())
	s, err := NewServer(cfg, r, quietLogger())
	if err != nil {
		t.Fatalf("starting the server: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- s.Run(ctx) }()
	t.Cleanup(func() {
		cancel()
		// Bounded: a test that already took Run's result from done must not hang here.
		select {
		case <-done:
		case <-time.After(2 * time.Second):
		}
		s.Close()
	})
	return r, s.LocalAddr().(*net.UDPAddr), done
}

func send(t *testing.T, to *net.UDPAddr, payload []byte) {
	t.Helper()
	c, err := net.DialUDP("udp", nil, to)
	if err != nil {
		t.Fatalf("dialling the server: %v", err)
	}
	defer c.Close()
	if _, err := c.Write(payload); err != nil {
		t.Fatalf("sending: %v", err)
	}
}

// Anyone who can reach the port can send anything, including a datagram larger than
// any frame. On Windows that once made the read fail, and a failed read stopped the
// relay — so one stray packet took every model's telemetry down with it.
func TestOversizedDatagramIsCountedNotFatal(t *testing.T) {
	r, addr, done := startServer(t)

	send(t, addr, make([]byte, 2000))
	send(t, addr, loadFixture(t, "hello"))
	send(t, addr, loadFixture(t, "data"))

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		select {
		case err := <-done:
			t.Fatalf("the relay stopped after an oversized datagram: %v", err)
		default:
		}
		if st := r.Stats(); st.Rows == 3 {
			if st.BadFrames != 1 {
				t.Errorf("bad_frames=%d, want 1 for the oversized datagram", st.BadFrames)
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("the frames after the oversized datagram never arrived: %+v", r.Stats())
}
