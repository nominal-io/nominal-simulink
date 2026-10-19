package proxy

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"

	"gopkg.in/yaml.v3"
)

// Profile is a connection profile from Nominal's config file: the file the Simulink block,
// the Python client, the nom CLI and nominal-matlab read too, so a computer set up for one
// is set up for all.
type Profile struct {
	BaseURL      string `yaml:"base_url"`
	Token        string `yaml:"token"`
	WorkspaceRID string `yaml:"workspace_rid"` // empty for the organization's default
}

// DefaultProfileFile is where Nominal's clients keep the config file:
// ~/.config/nominal/config.yml, or %USERPROFILE%\.config\nominal\config.yml on Windows.
func DefaultProfileFile() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".config", "nominal", "config.yml"), nil
}

// ReadProfile reads the profile name from the config file at path.
//
// Unknown keys are ignored, unlike in nmproxy's own file: the other clients share this one,
// and may add keys this release doesn't know.
func ReadProfile(path, name string) (Profile, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		// The message names the path already, so only the reason is worth adding.
		var pathErr *fs.PathError
		if errors.As(err, &pathErr) {
			err = pathErr.Err
		}
		return Profile{}, fmt.Errorf("reading the Nominal config file %s: %w. "+
			"Create it with a profile holding your API key and base URL, "+
			"or set profile_file to where yours is", path, err)
	}

	var file struct {
		Profiles map[string]*Profile `yaml:"profiles"`
	}
	if err := yaml.Unmarshal(raw, &file); err != nil {
		return Profile{}, fmt.Errorf("parsing the Nominal config file %s: %w", path, err)
	}

	p := file.Profiles[name]
	switch {
	case p == nil:
		return Profile{}, fmt.Errorf("the Nominal config file %s has no profile named %q. "+
			"Check profile, or add the profile to the file", path, name)
	case p.Token == "":
		return Profile{}, fmt.Errorf("profile %q in %s has no token. "+
			"Add your API key to it as token", name, path)
	case p.BaseURL == "":
		return Profile{}, fmt.Errorf("profile %q in %s has no base_url. "+
			"Add your instance's API URL to it as base_url", name, path)
	}
	return *p, nil
}
