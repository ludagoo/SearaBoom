// Package image fetches the live USB factory slot from the OTA server.
//
// Firmware is never embedded. The desktop binary asks
// https://searaboom.goossen.dev/api/factory (or --factory-url) for the
// current version and downloads those bins. A newer publish is picked up
// on the next refresh without rebuilding this program.
package image

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/layout"
)

const DefaultFactoryURL = "https://searaboom.goossen.dev"

type FileMeta struct {
	Filename string `json:"filename"`
	Offset   int    `json:"offset"`
}

type Manifest struct {
	Ready   bool       `json:"ready"`
	Version string     `json:"version"`
	Files   []FileMeta `json:"files"`
}

type Getter func(url string) ([]byte, error)

type Source struct {
	BaseURL  string
	CacheDir string
	Get      Getter
	Dir      string
	Version  string
}

func DefaultCacheDir() string {
	if env := os.Getenv("SEARABOOM_FACTORY_CACHE"); env != "" {
		return env
	}
	cache, err := os.UserCacheDir()
	if err != nil || cache == "" {
		cache = os.TempDir()
	}
	return filepath.Join(cache, "searaboom", "factory")
}

func HTTPGetter(userAgent string) Getter {
	client := &http.Client{Timeout: 60 * time.Second}
	return func(url string) ([]byte, error) {
		req, err := http.NewRequest(http.MethodGet, url, nil)
		if err != nil {
			return nil, err
		}
		if userAgent != "" {
			req.Header.Set("User-Agent", userAgent)
		}
		resp, err := client.Do(req)
		if err != nil {
			return nil, err
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			body, _ := io.ReadAll(io.LimitReader(resp.Body, 2048))
			return nil, fmt.Errorf("GET %s: %s %s", url, resp.Status, strings.TrimSpace(string(body)))
		}
		return io.ReadAll(resp.Body)
	}
}

func Validate(meta Manifest) error {
	byName := map[string]FileMeta{}
	for _, item := range meta.Files {
		byName[item.Filename] = item
	}
	for _, want := range layout.Files {
		rec, ok := byName[want.Filename]
		if !ok {
			return fmt.Errorf("factory.json missing %s", want.Filename)
		}
		if rec.Offset != 0 && rec.Offset != int(want.Offset) {
			return fmt.Errorf("%s offset %d != expected %d", want.Filename, rec.Offset, want.Offset)
		}
	}
	return nil
}

func ParseManifest(raw []byte) (Manifest, error) {
	var meta Manifest
	if err := json.Unmarshal(raw, &meta); err != nil {
		return Manifest{}, err
	}
	return meta, nil
}

func VersionFromDir(dir string) string {
	raw, err := os.ReadFile(filepath.Join(dir, "factory.json"))
	if err != nil {
		return ""
	}
	meta, err := ParseManifest(raw)
	if err != nil {
		return ""
	}
	return meta.Version
}

func LocalOverride(dir string) (*Source, error) {
	if dir == "" {
		return nil, fmt.Errorf("empty image dir")
	}
	if !layout.SlotReady(dir) {
		return nil, fmt.Errorf("incomplete local factory image in %s", dir)
	}
	metaPath := filepath.Join(dir, "factory.json")
	if raw, err := os.ReadFile(metaPath); err == nil {
		meta, err := ParseManifest(raw)
		if err != nil {
			return nil, err
		}
		if err := Validate(meta); err != nil {
			return nil, err
		}
		return &Source{Dir: dir, Version: meta.Version}, nil
	}
	return &Source{Dir: dir, Version: ""}, nil
}

func NewLive(baseURL, cacheDir string, get Getter) *Source {
	if baseURL == "" {
		baseURL = DefaultFactoryURL
	}
	if cacheDir == "" {
		cacheDir = DefaultCacheDir()
	}
	if get == nil {
		get = HTTPGetter("searaboom-factory-flasher")
	}
	return &Source{
		BaseURL:  strings.TrimRight(baseURL, "/"),
		CacheDir: cacheDir,
		Get:      get,
	}
}

// Refresh queries the live /api/factory catalog and downloads that
// version's bins. Cached files for the same version are reused. A newer
// server version replaces the in-memory slot without a flasher rebuild.
func (s *Source) Refresh() error {
	if s.Get == nil {
		return fmt.Errorf("no HTTP getter")
	}
	raw, err := s.Get(s.BaseURL + "/api/factory")
	if err != nil {
		return fmt.Errorf("live factory catalog: %w", err)
	}
	meta, err := ParseManifest(raw)
	if err != nil {
		return fmt.Errorf("factory.json: %w", err)
	}
	if !meta.Ready {
		return fmt.Errorf("factory image is not ready on the server")
	}
	if err := Validate(meta); err != nil {
		return err
	}
	version := meta.Version
	if version == "" {
		version = "unknown"
	}
	dest := filepath.Join(s.CacheDir, version)
	if layout.SlotReady(dest) {
		s.Dir = dest
		s.Version = version
		return nil
	}
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	pretty, _ := json.MarshalIndent(meta, "", "  ")
	if err := os.WriteFile(filepath.Join(dest, "factory.json"), append(pretty, '\n'), 0o644); err != nil {
		return err
	}
	for _, item := range layout.Files {
		body, err := s.Get(s.BaseURL + "/api/factory/" + item.Filename)
		if err != nil {
			return fmt.Errorf("download %s: %w", item.Filename, err)
		}
		if err := os.WriteFile(filepath.Join(dest, item.Filename), body, 0o644); err != nil {
			return err
		}
	}
	if !layout.SlotReady(dest) {
		return fmt.Errorf("incomplete factory download in %s", dest)
	}
	s.Dir = dest
	s.Version = version
	return nil
}

func (s *Source) Ready() bool {
	return s != nil && s.Dir != "" && layout.SlotReady(s.Dir)
}
