package proxy

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/nominal-io/nominal-simulink/nmproxy/frame"
)

// RunSpec is the run a session's HELLO asks for.
type RunSpec struct {
	Title      string
	DatasetRID string
	RefName    string      // the name the dataset goes under in the run
	Tags       []frame.Tag // the run shows the dataset's points with these
	StartNanos int64
	Shared     bool   // one run for every session with this title
	Session    uint64 // the session that asked, for the log
}

// Runs creates runs in Nominal and sets their end, and logs each one it does. Both calls
// block on the network, so the registry makes them off the receive loop.
type Runs interface {
	// Begin creates the run. With spec.Shared, it first looks for the newest run with
	// exactly that title and uses it instead.
	Begin(ctx context.Context, spec RunSpec) (rid string, err error)

	// End sets the run's end. With spec.Shared, it only ever moves the end later, so each
	// session sharing a run extends it.
	End(ctx context.Context, spec RunSpec, rid string, endNanos int64) error
}

// NominalRuns makes the requests the block makes in direct mode, from cstream/src/nm_runs.c,
// against the run service's REST API:
//
//	POST /scout/v1/run          create a run
//	PUT  /scout/v1/run/{rid}    set its end
//	POST /scout/v1/search-runs  find a shared run by title
//
// Written out rather than taken from nominal-api-go's generated client, which decodes the
// whole run in each response. Reading only the two fields used here means a newer server's
// responses still decode, and a run that was created is never reported as failed.
type NominalRuns struct {
	log       *slog.Logger
	baseURL   string
	token     string
	workspace string
	client    *http.Client

	// Held across a shared run's search and creation, so sessions that start together
	// don't each create one. shared remembers the runs found or created, since a search
	// may not yet list a run created a moment ago.
	mu     sync.Mutex
	shared map[string]string // title to RID
}

func NewNominalRuns(p Profile, log *slog.Logger) *NominalRuns {
	return &NominalRuns{
		log:       log,
		baseURL:   strings.TrimRight(p.BaseURL, "/"),
		token:     p.Token,
		workspace: p.WorkspaceRID,
		client:    &http.Client{Timeout: 15 * time.Second},
		shared:    map[string]string{},
	}
}

// A UtcTimestamp.
type utcTimestamp struct {
	SecondsSinceEpoch int64 `json:"secondsSinceEpoch"`
	OffsetNanoseconds int64 `json:"offsetNanoseconds"`
}

func timestamp(nanos int64) utcTimestamp {
	s, ns := nanos/1e9, nanos%1e9
	if ns < 0 { // before the epoch: the offset still counts forwards
		s, ns = s-1, ns+1e9
	}
	return utcTimestamp{SecondsSinceEpoch: s, OffsetNanoseconds: ns}
}

func (n *NominalRuns) Begin(ctx context.Context, spec RunSpec) (string, error) {
	if spec.Shared {
		n.mu.Lock()
		defer n.mu.Unlock()

		rid, ok := n.shared[spec.Title]
		if !ok {
			var err error
			if rid, err = n.find(ctx, spec.Title, nil); err != nil {
				return "", err
			}
		}
		if rid != "" {
			n.shared[spec.Title] = rid
			n.log.Info("run found", "run", rid, "title", spec.Title, "session", spec.Session)
			return rid, nil
		}
	} else {
		// A relay that restarts mid-execution sees the HELLO again with no session, and
		// would create a second run. The first one has this title and this start, which
		// every HELLO of the execution repeats, so it is found and used instead.
		rid, err := n.find(ctx, spec.Title, &spec.StartNanos)
		if err != nil {
			return "", err
		}
		if rid != "" {
			n.log.Info("run found", "run", rid, "title", spec.Title, "session", spec.Session)
			return rid, nil
		}
	}

	tags := map[string]string{}
	for _, t := range spec.Tags {
		tags[t.Key] = t.Value
	}
	type dataSource struct {
		DataSourceRid string            `json:"dataSourceRid"`
		SeriesTags    map[string]string `json:"seriesTags"`
	}
	body := struct {
		Title       string                `json:"title"`
		Description string                `json:"description"`
		StartTime   utcTimestamp          `json:"startTime"`
		DataSources map[string]dataSource `json:"dataSources"`
		Workspace   string                `json:"workspace,omitempty"`
	}{
		Title:     spec.Title,
		StartTime: timestamp(spec.StartNanos),
		DataSources: map[string]dataSource{
			spec.RefName: {DataSourceRid: spec.DatasetRID, SeriesTags: tags},
		},
		Workspace: n.workspace,
	}

	var run struct {
		Rid string `json:"rid"`
	}
	if err := n.do(ctx, http.MethodPost, "/scout/v1/run", body, &run); err != nil {
		return "", err
	}
	if run.Rid == "" {
		return "", fmt.Errorf("the response named no run")
	}
	if spec.Shared {
		n.shared[spec.Title] = run.Rid
	}
	n.log.Info("run created", "run", run.Rid, "title", spec.Title, "session", spec.Session)
	return run.Rid, nil
}

func (n *NominalRuns) End(ctx context.Context, spec RunSpec, rid string, endNanos int64) error {
	body := struct {
		EndTime         utcTimestamp `json:"endTime"`
		StrictOverwrite *bool        `json:"strictOverwrite,omitempty"`
	}{EndTime: timestamp(endNanos)}
	if spec.Shared {
		// Only move the end later, so a shorter session can't cut a shared run short.
		keep := false
		body.StrictOverwrite = &keep
	}
	if err := n.do(ctx, http.MethodPut, "/scout/v1/run/"+url.PathEscape(rid), body, nil); err != nil {
		return err
	}
	n.log.Info("run ended", "run", rid, "title", spec.Title, "session", spec.Session)
	return nil
}

// find returns the newest run titled exactly title, and starting at *start if start isn't
// nil, or "" if there is none. The search's exactMatch matches part of a title, ignoring
// case, so every page of results is read and filtered here until the run is found. The
// more runs have similar titles, the longer this takes.
func (n *NominalRuns) find(ctx context.Context, title string, start *int64) (string, error) {
	query := any(map[string]any{"type": "exactMatch", "exactMatch": title})
	if n.workspace != "" {
		query = map[string]any{"type": "and", "and": []any{
			query,
			map[string]any{"type": "workspace", "workspace": n.workspace},
		}}
	}

	token := ""
	for {
		body := map[string]any{
			"sort": map[string]any{
				"isDescending": true,
				"sortKey":      map[string]any{"type": "field", "field": "CREATED_AT"},
			},
			"pageSize": 100,
			"query":    query,
		}
		if token != "" {
			body["nextPageToken"] = token
		}

		var page struct {
			Results []struct {
				Rid       string       `json:"rid"`
				Title     string       `json:"title"`
				StartTime utcTimestamp `json:"startTime"`
			} `json:"results"`
			NextPageToken string `json:"nextPageToken"`
		}
		if err := n.do(ctx, http.MethodPost, "/scout/v1/search-runs", body, &page); err != nil {
			return "", err
		}
		for _, r := range page.Results {
			if r.Title == title && (start == nil || r.StartTime == timestamp(*start)) {
				return r.Rid, nil
			}
		}
		// A repeated token would loop forever, so it ends the search like no token.
		if page.NextPageToken == "" || page.NextPageToken == token {
			return "", nil
		}
		token = page.NextPageToken
	}
}

// do sends one request and decodes the answer into out, if out isn't nil.
func (n *NominalRuns) do(ctx context.Context, method, path string, body, out any) error {
	data, err := json.Marshal(body)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, method, n.baseURL+path, bytes.NewReader(data))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("Authorization", "Bearer "+n.token)

	resp, err := n.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	raw, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		// The body is a conjure error, whose errorInstanceId is what support needs.
		return fmt.Errorf("HTTP status %d: %.200s", resp.StatusCode, raw)
	}
	if out == nil {
		return nil
	}
	return json.Unmarshal(raw, out)
}

// LogRuns logs the runs it would make and makes none, for a dry run.
type LogRuns struct {
	Log *slog.Logger
}

func (l *LogRuns) Begin(_ context.Context, spec RunSpec) (string, error) {
	l.Log.Info("run (dry run: not created)", "title", spec.Title, "ref_name", spec.RefName,
		"tags", spec.Tags, "start", spec.StartNanos, "shared", spec.Shared,
		"session", spec.Session)
	return "dry-run", nil
}

func (l *LogRuns) End(_ context.Context, spec RunSpec, _ string, endNanos int64) error {
	l.Log.Info("run end (dry run: not set)", "title", spec.Title, "end", endNanos,
		"session", spec.Session)
	return nil
}
