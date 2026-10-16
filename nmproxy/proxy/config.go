package proxy

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"runtime"
	"time"

	"gopkg.in/yaml.v3"
)

func newBytesReader(b []byte) io.Reader { return bytes.NewReader(b) }

// Config is the daemon's entire configuration, loaded from a file.
//
// A file rather than environment variables, deliberately. A daemon's settings outlive
// any shell that started it, and env vars are awkward to inspect, awkward to set under
// a service manager, and invisible when you are trying to work out how a running proxy
// was configured. A file can be version-controlled (minus the token), diffed, and
// pointed at by a systemd unit.
//
// YAML rather than JSON because this file is written and read by people, and comments
// are the difference between a template that explains itself and one that needs the
// README open beside it.
type Config struct {
	// Listen is the UDP address to receive frames on, host:port. Bind to a specific
	// interface rather than 0.0.0.0 where you can: frames are unauthenticated, so the
	// listening interface is the primary access control.
	Listen string `yaml:"listen"`

	Nominal NominalConfig `yaml:"nominal"`

	// IdleTimeout is how long a configuration may go unheard before its stream is
	// closed and flushed. Hosts re-announce roughly every second, so anything above a
	// few seconds is safe; the default is generous because closing early would end a
	// run that is merely paused.
	IdleTimeout time.Duration `yaml:"idle_timeout"`

	// AllowedDatasets, when non-empty, restricts which dataset RIDs will be written to.
	//
	// Worth setting. Frames carry no authentication, so without this anyone able to
	// reach the listening port can write to any dataset this proxy's token permits —
	// and the token is usually broad. A list here bounds the damage to datasets you
	// intended.
	AllowedDatasets []string `yaml:"allowed_datasets"`

	// LogLevel is one of debug, info, warn, error.
	LogLevel string `yaml:"log_level"`

	// StatsInterval controls how often a summary line is logged. Zero disables it.
	StatsInterval time.Duration `yaml:"stats_interval"`

	// DryRun decodes and reports frames without writing anything to Nominal. The
	// fastest way to answer "are frames arriving and do they look right?" without
	// needing a valid token.
	DryRun bool `yaml:"dry_run"`
}

type NominalConfig struct {
	BaseURL string `yaml:"base_url"`

	// Token is a Nominal access token. This is the one secret in the file, which is
	// why LoadConfig warns about the file's permissions — see checkPermissions.
	Token string `yaml:"token"`
}

// DefaultConfig holds everything that has a sensible default, so a minimal file only
// needs a token and a dataset allowlist.
func DefaultConfig() Config {
	return Config{
		Listen:        "0.0.0.0:15400",
		IdleTimeout:   5 * time.Minute,
		LogLevel:      "info",
		StatsInterval: 30 * time.Second,
		Nominal: NominalConfig{
			BaseURL: "https://api.gov.nominal.io/api",
		},
	}
}

// LoadConfig reads and validates a config file. Unknown fields are an error rather than
// being ignored, so a typo in a key name fails loudly instead of silently leaving a
// setting at its default — which for AllowedDatasets would quietly remove a security
// control.
func LoadConfig(path string) (Config, []string, error) {
	cfg := DefaultConfig()
	var warnings []string

	raw, err := os.ReadFile(path)
	if err != nil {
		return cfg, nil, fmt.Errorf("reading %s: %w", path, err)
	}

	dec := yaml.NewDecoder(newBytesReader(raw))
	dec.KnownFields(true)
	if err := dec.Decode(&cfg); err != nil {
		return cfg, nil, fmt.Errorf("parsing %s: %w", path, err)
	}

	if cfg.Listen == "" {
		return cfg, nil, fmt.Errorf("%s: listen is required", path)
	}
	if !cfg.DryRun && cfg.Nominal.Token == "" {
		return cfg, nil, fmt.Errorf("%s: nominal.token is required unless dry_run is true", path)
	}
	if !cfg.DryRun && cfg.Nominal.BaseURL == "" {
		return cfg, nil, fmt.Errorf("%s: nominal.base_url is required", path)
	}
	if cfg.IdleTimeout < 0 {
		return cfg, nil, fmt.Errorf("%s: idle_timeout cannot be negative", path)
	}
	// An explicit zero used to mean the 5m default without saying so. Omitting the key
	// already gives the default, so a zero is a mistake worth reporting.
	if cfg.IdleTimeout == 0 {
		return cfg, nil, fmt.Errorf("%s: idle_timeout must be greater than zero; "+
			"leave it out to use the default of %s", path, DefaultConfig().IdleTimeout)
	}
	// Checked like an unknown key, and for the same reason: a typo here used to mean
	// info without a word.
	switch cfg.LogLevel {
	case "debug", "info", "warn", "error":
	default:
		return cfg, nil, fmt.Errorf("%s: log_level %q is not one of debug, info, warn, error",
			path, cfg.LogLevel)
	}

	if len(cfg.AllowedDatasets) == 0 && !cfg.DryRun {
		warnings = append(warnings,
			"allowed_datasets is empty: any sender that can reach the listening port "+
				"can write to any dataset this token permits")
	}
	if w := checkPermissions(path); w != "" {
		warnings = append(warnings, w)
	}

	return cfg, warnings, nil
}

// DatasetAllowed reports whether a RID may be written to.
func (c Config) DatasetAllowed(rid string) bool {
	if len(c.AllowedDatasets) == 0 {
		return true
	}
	for _, allowed := range c.AllowedDatasets {
		if allowed == rid {
			return true
		}
	}
	return false
}

// checkPermissions warns if the config is readable by anyone but its owner. The file
// holds a token, so this is worth saying out loud rather than trusting deployment to
// have got it right.
//
// Skipped on Windows, where the POSIX mode bits do not describe the real ACL and would
// produce a misleading warning either way.
func checkPermissions(path string) string {
	if runtime.GOOS == "windows" {
		return ""
	}
	info, err := os.Stat(path)
	if err != nil {
		return ""
	}
	if mode := info.Mode().Perm(); mode&0o077 != 0 {
		return fmt.Sprintf("%s is mode %#o and holds an access token; consider chmod 600",
			path, mode)
	}
	return ""
}
