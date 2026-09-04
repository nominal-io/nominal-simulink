// Command nmproxy receives telemetry frames from Simulink hosts and writes them to
// Nominal.
//
// It exists so that a host with no performance headroom can still stream: in proxy mode
// the host sends compact datagrams and this process does the protobuf encoding, TLS and
// HTTP that the host would otherwise have paid for. The host also stops needing a
// credential — the dataset RID travels in the frames, the token lives only here.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/nominal-io/nominal-simulink/nmproxy/proxy"
)

func main() {
	var (
		configPath  = flag.String("config", "", "path to nmproxy.yaml (required)")
		showVersion = flag.Bool("version", false, "print the protocol version and exit")
	)
	flag.Usage = usage
	flag.Parse()

	if *showVersion {
		fmt.Printf("nmproxy, frame protocol version %d\n", frameVersion)
		return
	}
	if *configPath == "" {
		usage()
		os.Exit(2)
	}

	if err := run(*configPath); err != nil {
		fmt.Fprintf(os.Stderr, "nmproxy: %v\n", err)
		os.Exit(1)
	}
}

func run(configPath string) error {
	cfg, warnings, err := proxy.LoadConfig(configPath)
	if err != nil {
		return err
	}

	log := newLogger(cfg.LogLevel)
	for _, w := range warnings {
		log.Warn(w)
	}

	// The dry-run writer is not only a test double: it answers "are frames arriving and
	// do they decode sensibly?" without a token or any egress, which is the first thing
	// worth knowing when a deployment is not working.
	var writer proxy.Writer
	if cfg.DryRun {
		log.Info("dry run: frames will be decoded and reported, nothing written")
		writer = &proxy.LogWriter{Log: log, Verbose: cfg.LogLevel == "debug"}
	} else {
		nw, err := proxy.NewNominalWriter(cfg, log)
		if err != nil {
			return err
		}
		defer nw.Close()
		writer = nw
		log.Info("writing to Nominal", "base_url", cfg.Nominal.BaseURL)
	}

	registry := proxy.NewRegistry(writer, cfg, log)
	server, err := proxy.NewServer(cfg, registry, log)
	if err != nil {
		return err
	}
	defer server.Close()

	log.Info("listening", "addr", server.LocalAddr().String(),
		"idle_timeout", cfg.IdleTimeout,
		"allowed_datasets", len(cfg.AllowedDatasets))

	// Cancel on SIGINT/SIGTERM so streams are flushed on shutdown rather than losing
	// whatever the writer had buffered.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := server.Run(ctx); err != nil {
		return err
	}

	st := registry.Stats()
	log.Info("shutting down",
		"rows", st.Rows, "gaps", st.Gaps, "reorders", st.Reorders,
		"bad_frames", st.BadFrames, "wrong_version", st.WrongVersion,
		"refused", st.Refused, "write_errors", st.WriteErrors)
	return nil
}

func newLogger(level string) *slog.Logger {
	var l slog.Level
	switch level {
	case "debug":
		l = slog.LevelDebug
	case "warn":
		l = slog.LevelWarn
	case "error":
		l = slog.LevelError
	default:
		l = slog.LevelInfo
	}
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: l}))
}

func usage() {
	fmt.Fprint(os.Stderr, `nmproxy — receives Simulink telemetry frames and writes them to Nominal

Usage:
  nmproxy -config /etc/nominal/nmproxy.yaml

Configuration is a file, not environment variables: a daemon's settings outlive the
shell that started it, and a file can be inspected, diffed and referenced from a
service unit. Start from nmproxy.example.yaml.

Flags:
`)
	flag.PrintDefaults()
}
