package proxy

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func loadYAML(t *testing.T, yaml string) error {
	t.Helper()
	path := filepath.Join(t.TempDir(), "nmproxy.yaml")
	if err := os.WriteFile(path, []byte(yaml), 0o600); err != nil {
		t.Fatal(err)
	}
	_, _, err := LoadConfig(path)
	return err
}

// Settings that used to be quietly replaced by a default are now reported, the same
// way an unknown key already was.
func TestLoadConfigRejectsSettingsItWouldIgnore(t *testing.T) {
	for _, tc := range []struct {
		name, yaml, want string
	}{
		{"unknown log_level", "dry_run: true\nlog_level: verbose\n", "log_level"},
		{"explicit zero idle_timeout", "dry_run: true\nidle_timeout: 0s\n", "idle_timeout"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			err := loadYAML(t, tc.yaml)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Errorf("got %v, want an error naming %s", err, tc.want)
			}
		})
	}
}

func TestLoadConfigKeepsDefaultsForOmittedKeys(t *testing.T) {
	if err := loadYAML(t, "dry_run: true\n"); err != nil {
		t.Errorf("a minimal dry-run config was refused: %v", err)
	}
}
