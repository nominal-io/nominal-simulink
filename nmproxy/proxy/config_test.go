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

// profileConfig writes a Nominal config file and an nmproxy.yaml that points at it, and
// loads them.
func profileConfig(t *testing.T, profiles, settings string) (Config, error) {
	t.Helper()
	dir := t.TempDir()
	file := filepath.Join(dir, "config.yml")
	if err := os.WriteFile(file, []byte("version: 2\nprofiles:\n"+profiles), 0o600); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "nmproxy.yaml")
	yaml := "profile_file: " + strings.ReplaceAll(file, `\`, `/`) + "\n" + settings
	if err := os.WriteFile(path, []byte(yaml), 0o600); err != nil {
		t.Fatal(err)
	}
	cfg, _, err := LoadConfig(path)
	return cfg, err
}

// The example ships in every release archive, so it has to load as it stands, apart from
// the profile file it points at.
func TestTheExampleConfigLoads(t *testing.T) {
	example, err := os.ReadFile(filepath.Join("..", "nmproxy.example.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	settings := strings.Replace(string(example), "profile_file: \"\"\n", "", 1)
	if settings == string(example) {
		t.Fatal(`the example no longer has the line profile_file: ""`)
	}
	if _, err := profileConfig(t, "  default:\n    base_url: u\n    token: k\n", settings); err != nil {
		t.Errorf("the example config was refused: %v", err)
	}
}

func TestLoadConfigReadsTheProfile(t *testing.T) {
	cfg, err := profileConfig(t, `
  default:
    base_url: https://api.example.test/api
    token: "key-one"
    workspace_rid: null
  staging:
    base_url: https://api.staging.example.test/api
    token: key-two
    workspace_rid: ri.security.test.workspace.1
`, "profile: staging\n")
	if err != nil {
		t.Fatal(err)
	}
	want := Profile{
		BaseURL:      "https://api.staging.example.test/api",
		Token:        "key-two",
		WorkspaceRID: "ri.security.test.workspace.1",
	}
	if cfg.Nominal != want {
		t.Errorf("profile = %+v, want %+v", cfg.Nominal, want)
	}
}

func TestLoadConfigSaysWhatIsWrongWithTheProfile(t *testing.T) {
	for _, tc := range []struct {
		name, profiles, settings, want string
	}{
		{"no such profile", "  default:\n    base_url: u\n    token: k\n", "profile: prod\n",
			`has no profile named "prod"`},
		{"no token", "  default:\n    base_url: u\n    token: null\n", "",
			`profile "default" in`},
		{"no base_url", "  default:\n    token: k\n", "", "has no base_url"},
		{"an empty profile name", "  default:\n    base_url: u\n    token: k\n", "profile: \"\"\n",
			"profile is empty"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := profileConfig(t, tc.profiles, tc.settings)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Errorf("got %v, want an error containing %q", err, tc.want)
			}
		})
	}

	err := loadYAML(t, "profile_file: "+strings.ReplaceAll(filepath.Join(t.TempDir(), "none.yml"), `\`, `/`)+"\n")
	if err == nil || !strings.Contains(err.Error(), "reading the Nominal config file") {
		t.Errorf("a missing config file gave %v", err)
	}
}
