package proxy

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/nominal-io/nominal-simulink/nmproxy/frame"
)

// The HELLO fixture asks for a run of its own, titled "Model A, execution 7", starting at
// 1000000000 and showing both tags. The END fixture ends it at 1000002000.
const (
	fixtureRunStart = int64(1000000000)
	fixtureRunEnd   = int64(1000002000)
)

// fakeRuns records what the registry asks of it. Begin can be made to fail, or to wait.
type fakeRuns struct {
	mu        sync.Mutex
	begins    []RunSpec
	ends      []int64
	failBegin error
	hold      chan struct{} // when set, Begin waits for it to close
}

func (f *fakeRuns) Begin(_ context.Context, spec RunSpec) (string, error) {
	if f.hold != nil {
		<-f.hold
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	f.begins = append(f.begins, spec)
	if f.failBegin != nil {
		return "", f.failBegin
	}
	return fmt.Sprintf("ri.scout.test.run.%d", len(f.begins)), nil
}

func (f *fakeRuns) End(_ context.Context, _ RunSpec, _ string, endNanos int64) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.ends = append(f.ends, endNanos)
	return nil
}

func (f *fakeRuns) calls() ([]RunSpec, []int64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]RunSpec(nil), f.begins...), append([]int64(nil), f.ends...)
}

func newRunRegistry(t *testing.T, runs Runs, log *slog.Logger) (*Registry, *LogWriter) {
	t.Helper()
	w := &LogWriter{Log: quietLogger()}
	return NewRegistry(w, runs, Config{IdleTimeout: 10 * time.Second}, log), w
}

// withRun returns a copy of the HELLO fixture asking for another kind of run. run_mode
// sits before run_start(8), run_tag_count(2), the title and the ref name.
func withRun(t *testing.T, hello []byte, mode uint8, tagCount int) []byte {
	t.Helper()
	h, err := frame.DecodeHello(hello)
	if err != nil {
		t.Fatalf("decoding the HELLO fixture: %v", err)
	}
	out := append([]byte(nil), hello...)
	at := len(out) - (2 + len(h.Run.RefName)) - (2 + len(h.Run.Title)) - 2 - 8 - 1
	out[at] = mode
	binary.LittleEndian.PutUint16(out[at+9:at+11], uint16(tagCount))
	return out
}

// HELLO repeats every second and END is sent several times, so a relay that acted on
// each would create a run a second and end it over and over.
func TestRepeatedHelloAndEndMakeOneRun(t *testing.T) {
	runs := &fakeRuns{}
	r, w := newRunRegistry(t, runs, quietLogger())
	now := time.Now()

	for i := 0; i < 5; i++ {
		r.Handle(loadFixture(t, "hello"), now)
	}
	r.Handle(loadFixture(t, "data"), now)
	for i := 0; i < 3; i++ {
		r.Handle(loadFixture(t, "end"), now)
	}
	r.runsActive.Wait()

	begins, ends := runs.calls()
	if len(begins) != 1 {
		t.Fatalf("created %d runs, want 1", len(begins))
	}
	want := RunSpec{
		Title:      "Model A, execution 7",
		DatasetRID: fixtureRID,
		RefName:    "default",
		Tags:       []frame.Tag{{Key: "model", Value: "A"}, {Key: "execution-id", Value: "7"}},
		StartNanos: fixtureRunStart,
		Shared:     false,
		Session:    0x1122334455667788,
	}
	if !reflect.DeepEqual(begins[0], want) {
		t.Errorf("run = %+v, want %+v", begins[0], want)
	}
	if !reflect.DeepEqual(ends, []int64{fixtureRunEnd}) {
		t.Errorf("ends = %v, want one at %d", ends, fixtureRunEnd)
	}
	if got := w.Rows(); got != 3 {
		t.Errorf("wrote %d rows, want 3", got)
	}
}

// A run per model shows the points with fewer tags than the HELLO carries, so every
// execution's points are in it.
func TestASharedRunShowsTheTagsTheHelloSays(t *testing.T) {
	runs := &fakeRuns{}
	r, _ := newRunRegistry(t, runs, quietLogger())

	r.Handle(withRun(t, loadFixture(t, "hello"), frame.RunShared, 1), time.Now())
	r.Close()

	begins, _ := runs.calls()
	if len(begins) != 1 || !begins[0].Shared ||
		!reflect.DeepEqual(begins[0].Tags, []frame.Tag{{Key: "model", Value: "A"}}) {
		t.Errorf("runs = %+v, want one shared run showing model=A", begins)
	}
}

func TestNoRunWhenTheHelloAsksForNone(t *testing.T) {
	runs := &fakeRuns{}
	r, _ := newRunRegistry(t, runs, quietLogger())

	r.Handle(withRun(t, loadFixture(t, "hello"), frame.RunNone, 0), time.Now())
	r.Close()

	if begins, _ := runs.calls(); len(begins) != 0 {
		t.Errorf("created %d runs, want none", len(begins))
	}
}

// Begin takes as long as Nominal does to answer. An END that arrives first still ends
// the run, once it exists.
func TestAnEndBeforeTheRunExistsStillEndsIt(t *testing.T) {
	runs := &fakeRuns{hold: make(chan struct{})}
	r, _ := newRunRegistry(t, runs, quietLogger())
	now := time.Now()

	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "end"), now)
	close(runs.hold)
	r.runsActive.Wait()

	if _, ends := runs.calls(); !reflect.DeepEqual(ends, []int64{fixtureRunEnd}) {
		t.Errorf("ends = %v, want one at %d", ends, fixtureRunEnd)
	}
}

// When every END is lost, the run still ends, at the newest row, once the session goes
// quiet.
func TestARunEndsWhenItsSessionGoesIdle(t *testing.T) {
	runs := &fakeRuns{}
	r, _ := newRunRegistry(t, runs, quietLogger())
	start := time.Now()

	r.Handle(loadFixture(t, "hello"), start)
	r.Handle(loadFixture(t, "data"), start)
	r.Sweep(start.Add(5 * time.Second))
	if _, ends := runs.calls(); len(ends) != 0 {
		t.Fatalf("ended the run while its session was still going")
	}

	r.Sweep(start.Add(30 * time.Second))
	r.runsActive.Wait()
	if _, ends := runs.calls(); !reflect.DeepEqual(ends, []int64{fixtureRunEnd}) {
		t.Errorf("ends = %v, want one at the newest row, %d", ends, fixtureRunEnd)
	}
}

// Nominal refuses a run whose end isn't after its start.
func TestARunWithNoRowsEndsJustAfterItsStart(t *testing.T) {
	runs := &fakeRuns{}
	r, _ := newRunRegistry(t, runs, quietLogger())

	r.Handle(loadFixture(t, "hello"), time.Now())
	r.Close()

	if _, ends := runs.calls(); !reflect.DeepEqual(ends, []int64{fixtureRunStart + 1}) {
		t.Errorf("ends = %v, want one at %d", ends, fixtureRunStart+1)
	}
}

func TestARunThatCantBeCreatedOnlyCostsTheRun(t *testing.T) {
	rec := &logRecorder{}
	runs := &fakeRuns{failBegin: errors.New("HTTP status 403: forbidden")}
	r, w := newRunRegistry(t, runs, slog.New(rec))
	now := time.Now()

	r.Handle(loadFixture(t, "hello"), now)
	r.Handle(loadFixture(t, "data"), now)
	r.Handle(loadFixture(t, "end"), now)
	r.runsActive.Wait()

	if got := w.Rows(); got != 3 {
		t.Errorf("wrote %d rows, want 3", got)
	}
	if got := rec.count("could not create the run"); got != 1 {
		t.Errorf("logged the failure %d times, want 1", got)
	}
	if _, ends := runs.calls(); len(ends) != 0 {
		t.Errorf("ended a run that was never created")
	}
}

// --- the requests NominalRuns makes, against a fake Nominal

type request struct {
	method, path, auth string
	body               map[string]any
}

// fakeNominal answers each request with the next of replies, and keeps the requests.
type fakeNominal struct {
	mu       sync.Mutex
	replies  []string // "status body"
	requests []request
}

func (f *fakeNominal) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	raw, _ := io.ReadAll(req.Body)
	var body map[string]any
	_ = json.Unmarshal(raw, &body)

	f.mu.Lock()
	f.requests = append(f.requests, request{req.Method, req.URL.Path, req.Header.Get("Authorization"), body})
	reply := "500 {}"
	if len(f.replies) > 0 {
		reply, f.replies = f.replies[0], f.replies[1:]
	}
	f.mu.Unlock()

	var status int
	fmt.Sscanf(reply, "%d", &status)
	w.WriteHeader(status)
	_, _ = io.WriteString(w, reply[strings.Index(reply, " ")+1:])
}

func newNominalRuns(t *testing.T, workspace string, replies ...string) (*NominalRuns, *fakeNominal) {
	t.Helper()
	fake := &fakeNominal{replies: replies}
	srv := httptest.NewServer(fake)
	t.Cleanup(srv.Close)
	return NewNominalRuns(Profile{BaseURL: srv.URL + "/", Token: "test-key", WorkspaceRID: workspace},
		quietLogger()), fake
}

// asJSON turns v into what a JSON body decodes to, for comparing with a request's body.
func asJSON(t *testing.T, v any) any {
	t.Helper()
	raw, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	var out any
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatal(err)
	}
	return out
}

var testSpec = RunSpec{
	Title:      "Model A, execution 7",
	DatasetRID: fixtureRID,
	RefName:    "default",
	Tags:       []frame.Tag{{Key: "model", Value: "A"}, {Key: "execution-id", Value: "7"}},
	StartNanos: 1_700_000_000_250_000_000,
}

func TestCreatingARun(t *testing.T) {
	n, fake := newNominalRuns(t, "", `200 {"results":[]}`,
		`200 {"rid":"ri.scout.test.run.1","title":"x"}`)

	rid, err := n.Begin(context.Background(), testSpec)
	if err != nil || rid != "ri.scout.test.run.1" {
		t.Fatalf("Begin = %q, %v", rid, err)
	}
	if len(fake.requests) != 2 || fake.requests[0].path != "/scout/v1/search-runs" {
		t.Fatalf("requests = %+v, want a search and then a create", fake.requests)
	}
	got := fake.requests[1]
	if got.method != "POST" || got.path != "/scout/v1/run" || got.auth != "Bearer test-key" {
		t.Errorf("request = %s %s with %q", got.method, got.path, got.auth)
	}
	want := asJSON(t, map[string]any{
		"title":       "Model A, execution 7",
		"description": "",
		"startTime":   map[string]any{"secondsSinceEpoch": 1700000000, "offsetNanoseconds": 250000000},
		"dataSources": map[string]any{"default": map[string]any{
			"dataSourceRid": fixtureRID,
			"seriesTags":    map[string]any{"model": "A", "execution-id": "7"},
		}},
	})
	if !reflect.DeepEqual(asJSON(t, got.body), want) {
		t.Errorf("body = %v, want %v", got.body, want)
	}
}

func TestEndingARun(t *testing.T) {
	n, fake := newNominalRuns(t, "", "200 {}", "200 {}")
	ctx := context.Background()

	if err := n.End(ctx, testSpec, "ri.scout.test.run.1", 1_700_000_005_000_000_000); err != nil {
		t.Fatal(err)
	}
	shared := testSpec
	shared.Shared = true
	if err := n.End(ctx, shared, "ri.scout.test.run.1", 1_700_000_005_000_000_000); err != nil {
		t.Fatal(err)
	}

	end := map[string]any{"secondsSinceEpoch": 1700000005, "offsetNanoseconds": 0}
	if got := fake.requests[0]; got.method != "PUT" || got.path != "/scout/v1/run/ri.scout.test.run.1" ||
		!reflect.DeepEqual(asJSON(t, got.body), asJSON(t, map[string]any{"endTime": end})) {
		t.Errorf("request = %s %s %v", got.method, got.path, got.body)
	}
	// A shared run's end only moves later, so a shorter execution can't cut it short.
	if got := fake.requests[1].body; !reflect.DeepEqual(asJSON(t, got),
		asJSON(t, map[string]any{"endTime": end, "strictOverwrite": false})) {
		t.Errorf("shared body = %v", got)
	}
}

// The search matches part of a title, ignoring case, so only an exact title counts. The
// run found is then remembered, since a search may not list a run created a moment ago.
func TestASharedRunIsFoundByItsExactTitle(t *testing.T) {
	n, fake := newNominalRuns(t, "",
		`200 {"results":[{"rid":"ri.scout.test.run.9","title":"model a"},`+
			`{"rid":"ri.scout.test.run.8","title":"Model A"}]}`)
	spec := testSpec
	spec.Title, spec.Shared = "Model A", true

	for i := 0; i < 2; i++ {
		rid, err := n.Begin(context.Background(), spec)
		if err != nil || rid != "ri.scout.test.run.8" {
			t.Fatalf("Begin = %q, %v, want the run titled exactly Model A", rid, err)
		}
	}
	if len(fake.requests) != 1 || fake.requests[0].path != "/scout/v1/search-runs" {
		t.Errorf("requests = %+v, want one search", fake.requests)
	}
}

// A relay restarted mid-execution gets the HELLO again and knows nothing of the run it
// made before. The run with the HELLO's title and start is that run, so it is used rather
// than a second one created, and the END then ends it. A run with the same title but
// another start belongs to another execution with a fixed Run title, and is left alone.
func TestARestartedRelayReusesTheExecutionsRun(t *testing.T) {
	sameStart := timestamp(testSpec.StartNanos)
	found := fmt.Sprintf(`200 {"results":[{"rid":"ri.scout.test.run.3","title":%q,`+
		`"startTime":{"secondsSinceEpoch":%d,"offsetNanoseconds":%d}}]}`,
		testSpec.Title, sameStart.SecondsSinceEpoch, sameStart.OffsetNanoseconds)
	n, fake := newNominalRuns(t, "", found)

	rid, err := n.Begin(context.Background(), testSpec)
	if err != nil || rid != "ri.scout.test.run.3" {
		t.Fatalf("Begin = %q, %v, want the run with this title and start", rid, err)
	}
	if len(fake.requests) != 1 {
		t.Errorf("made %d requests, want one search and no create", len(fake.requests))
	}

	otherStart := fmt.Sprintf(`200 {"results":[{"rid":"ri.scout.test.run.3","title":%q,`+
		`"startTime":{"secondsSinceEpoch":%d,"offsetNanoseconds":0}}]}`,
		testSpec.Title, sameStart.SecondsSinceEpoch+60)
	n, fake = newNominalRuns(t, "", otherStart, `200 {"rid":"ri.scout.test.run.4"}`)

	rid, err = n.Begin(context.Background(), testSpec)
	if err != nil || rid != "ri.scout.test.run.4" {
		t.Fatalf("Begin = %q, %v, want a new run", rid, err)
	}
	if len(fake.requests) != 2 {
		t.Errorf("made %d requests, want a search and a create", len(fake.requests))
	}
}

// The search returns a page at a time, and runs with similar titles can fill many pages
// ahead of the one wanted: every "Model A, execution N" contains "Model A". A search that
// stopped at the first page created a second "Model A" after a hundred executions.
func TestASharedRunIsFoundOnALaterPage(t *testing.T) {
	n, fake := newNominalRuns(t, "",
		`200 {"results":[{"rid":"ri.scout.test.run.9","title":"Model A, execution 9"}],`+
			`"nextPageToken":"page-2"}`,
		`200 {"results":[{"rid":"ri.scout.test.run.8","title":"Model A"}],`+
			`"nextPageToken":null}`)
	spec := testSpec
	spec.Title, spec.Shared = "Model A", true

	rid, err := n.Begin(context.Background(), spec)
	if err != nil || rid != "ri.scout.test.run.8" {
		t.Fatalf("Begin = %q, %v, want the run on the second page", rid, err)
	}
	if len(fake.requests) != 2 {
		t.Fatalf("made %d requests, want two searches", len(fake.requests))
	}
	if _, has := fake.requests[0].body["nextPageToken"]; has {
		t.Error("the first search carried a page token")
	}
	if got := fake.requests[1].body["nextPageToken"]; got != "page-2" {
		t.Errorf("the second search carried token %v, want page-2", got)
	}
}

func TestASharedRunIsCreatedInTheWorkspaceWhenThereIsNone(t *testing.T) {
	n, fake := newNominalRuns(t, "ri.security.test.workspace.1",
		`200 {"results":[]}`, `200 {"rid":"ri.scout.test.run.2"}`)
	spec := testSpec
	spec.Shared = true

	rid, err := n.Begin(context.Background(), spec)
	if err != nil || rid != "ri.scout.test.run.2" {
		t.Fatalf("Begin = %q, %v", rid, err)
	}
	search := asJSON(t, fake.requests[0].body).(map[string]any)
	wantQuery := asJSON(t, map[string]any{"type": "and", "and": []any{
		map[string]any{"type": "exactMatch", "exactMatch": spec.Title},
		map[string]any{"type": "workspace", "workspace": "ri.security.test.workspace.1"},
	}})
	if !reflect.DeepEqual(search["query"], wantQuery) {
		t.Errorf("query = %v, want %v", search["query"], wantQuery)
	}
	if got := fake.requests[1].body["workspace"]; got != "ri.security.test.workspace.1" {
		t.Errorf("created in workspace %v", got)
	}
}

func TestARefusedRequestSaysWhy(t *testing.T) {
	n, _ := newNominalRuns(t, "", `403 {"errorCode":"PERMISSION_DENIED"}`)

	_, err := n.Begin(context.Background(), testSpec)
	if err == nil || !strings.Contains(err.Error(), "HTTP status 403") ||
		!strings.Contains(err.Error(), "PERMISSION_DENIED") {
		t.Errorf("err = %v, want the status and the body", err)
	}
}

func TestTimestampsBeforeTheEpoch(t *testing.T) {
	if got := timestamp(-1); got != (utcTimestamp{-1, 999_999_999}) {
		t.Errorf("timestamp(-1) = %+v, want one nanosecond before the epoch", got)
	}
}
