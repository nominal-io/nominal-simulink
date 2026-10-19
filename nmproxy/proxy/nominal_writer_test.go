package proxy

import (
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// newRejectingRegistry wires a registry to the real Nominal client, pointed at a local
// server that answers every write with 401, as Nominal does for a bad API key.
func newRejectingRegistry(t *testing.T) (*Registry, *logRecorder) {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
	}))
	t.Cleanup(srv.Close)

	rec := &logRecorder{}
	log := slog.New(rec)
	cfg := DefaultConfig()
	cfg.Nominal.BaseURL = srv.URL
	cfg.Nominal.Token = "not-a-real-key"
	cfg.IdleTimeout = time.Minute

	w, err := NewNominalWriter(cfg, log)
	if err != nil {
		t.Fatalf("creating the writer: %v", err)
	}
	t.Cleanup(func() { _ = w.Close() })
	return NewRegistry(w, nil, cfg, log), rec
}

// A write that fails after Push returned is counted, and the stream's close says it had
// errors instead of reporting a clean "stream closed".
func TestFailedWritesAreCountedAndReportedAtClose(t *testing.T) {
	r, rec := newRejectingRegistry(t)
	now := time.Now()
	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)

	// The client sends on a 500 ms timer, so the failure arrives some time after Push.
	deadline := time.Now().Add(5 * time.Second)
	for r.Stats().WriteErrors == 0 {
		if time.Now().After(deadline) {
			t.Fatal("the rejected write was never counted in write_errors")
		}
		time.Sleep(10 * time.Millisecond)
	}

	r.Close()
	if got := rec.countExact("stream closed with errors"); got != 1 {
		t.Errorf(`logged "stream closed with errors" %d times, want 1`, got)
	}
	if got := rec.countExact("stream closed"); got != 0 {
		t.Errorf(`logged "stream closed" %d times for a stream whose writes failed`, got)
	}
}

// Rows still buffered at Close go out in a final batch. If that batch fails, it should
// be counted like any other failed write.
func TestFailedFinalBatchIsCounted(t *testing.T) {
	t.Skip("nominal-streaming-go discards errors from the batch it sends during Close: " +
		"batcher.close sets closed before its final flush, and reportError drops errors " +
		"once closed is set. Remove this skip once the client delivers them on errCh.")

	r, rec := newRejectingRegistry(t)
	now := time.Now()
	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)

	// Close before the client's 500 ms timer, so the rows go out in the final batch.
	r.Close()
	if st := r.Stats(); st.WriteErrors == 0 {
		t.Error("the rejected final batch was not counted in write_errors")
	}
	if got := rec.countExact("stream closed"); got != 0 {
		t.Errorf(`logged "stream closed" %d times for a stream whose final batch failed`, got)
	}
}
