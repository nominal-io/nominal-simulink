package proxy

import (
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

// newAcceptingWriter wires a writer to the real Nominal client, pointed at a local server
// that accepts every write and counts the requests that carried rows.
func newAcceptingWriter(t *testing.T) (*NominalWriter, *logRecorder, *atomic.Int32) {
	t.Helper()
	var requests atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if body, _ := io.ReadAll(r.Body); len(body) > 0 {
			requests.Add(1)
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	t.Cleanup(srv.Close)

	rec := &logRecorder{}
	cfg := DefaultConfig()
	cfg.Nominal.BaseURL = srv.URL
	cfg.Nominal.Token = "not-a-real-key"
	w, err := NewNominalWriter(cfg, slog.New(rec))
	if err != nil {
		t.Fatalf("creating the writer: %v", err)
	}
	return w, rec, &requests
}

func openOrFail(t *testing.T, w *NominalWriter, rid string, tags map[string]string) Stream {
	t.Helper()
	s, err := w.Open(rid, []string{"a", "b", "c"}, tags, nil)
	if err != nil {
		t.Fatalf("opening a stream for %s: %v", rid, err)
	}
	return s
}

// Every execution of a model is a new channel list, because its execution ID is a tag.
// A stream per channel list made a connection to Nominal per execution. The channel
// lists of a dataset share one stream, which closes once the last of them is closed.
func TestChannelListsOfADatasetShareOneStream(t *testing.T) {
	w, rec, _ := newAcceptingWriter(t)
	defer w.Close()

	first := openOrFail(t, w, fixtureRID, map[string]string{"execution-id": "7"})
	second := openOrFail(t, w, fixtureRID, map[string]string{"execution-id": "8"})
	other := openOrFail(t, w, "ri.catalog.test.dataset.xyz", nil)
	if got := rec.countExact("stream opened"); got != 2 {
		t.Fatalf("opened %d streams for three channel lists on two datasets, want 2", got)
	}

	if err := first.Close(); err != nil {
		t.Fatalf("closing the first channel list: %v", err)
	}
	if got := rec.countExact("stream closed"); got != 0 {
		t.Errorf("closed %d streams while a channel list still used the dataset, want 0", got)
	}
	if err := second.Close(); err != nil {
		t.Fatalf("closing the second channel list: %v", err)
	}
	if got := rec.countExact("stream closed"); got != 1 {
		t.Errorf("closed %d streams after the dataset's last channel list, want 1", got)
	}
	_ = other.Close()
}

// The client never forgets a channel and tag pair, so a stream shared by a campaign
// would grow with every execution. Past the limit, the writer moves the channel lists
// still open onto a fresh stream, and rows pushed before and after still land.
func TestAStreamIsReopenedPastTheChannelLimit(t *testing.T) {
	w, rec, requests := newAcceptingWriter(t)
	w.maxChannelStreams = 6

	first := openOrFail(t, w, fixtureRID, map[string]string{"execution-id": "7"})
	second := openOrFail(t, w, fixtureRID, map[string]string{"execution-id": "8"})
	if err := first.Push(1000, []float64{1, 2, 3}); err != nil {
		t.Fatalf("pushing before the limit: %v", err)
	}

	third := openOrFail(t, w, fixtureRID, map[string]string{"execution-id": "9"})
	if got := rec.countExact("stream reopened"); got != 1 {
		t.Fatalf("reopened %d times when the third channel list passed the limit, want 1", got)
	}
	for i, s := range []Stream{first, second, third} {
		if err := s.Push(2000, []float64{1, 2, 3}); err != nil {
			t.Fatalf("pushing on channel list %d after the reopen: %v", i, err)
		}
	}

	for _, s := range []Stream{first, second, third} {
		if err := s.Close(); err != nil {
			t.Fatalf("closing: %v", err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatalf("closing the writer: %v", err)
	}
	// The replaced stream's rows in its final batch, and the fresh stream's in its own.
	if got := requests.Load(); got < 2 {
		t.Errorf("Nominal received %d requests with rows, want at least 2", got)
	}
	if got := rec.countExact("stream closed"); got != 1 {
		t.Errorf("logged %d closes, want 1: the replaced stream closes without one", got)
	}
}

// Through the registry: two executions of one model are two channel lists for one
// dataset, and open one stream between them.
func TestExecutionsOfAModelShareTheDatasetsStream(t *testing.T) {
	r, rec := newRejectingRegistry(t)
	now := time.Now()
	hello := loadFixture(t, "hello")
	r.Handle(hello, now)
	r.Handle(withExecutionID(t, hello, "8"), now)

	if st := r.Stats(); st.Configs != 2 {
		t.Fatalf("configs=%d, want 2", st.Configs)
	}
	if got := rec.countExact("stream opened"); got != 1 {
		t.Errorf("opened %d streams for two executions of one model, want 1", got)
	}

	r.Close()
	if got := rec.countExact("stream closed"); got != 1 {
		t.Errorf("closed %d streams at shutdown, want 1", got)
	}
}

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
