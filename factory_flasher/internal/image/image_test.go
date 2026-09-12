package image

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/layout"
)

func TestDefaultURLHasNoPinnedFirmware(t *testing.T) {
	if DefaultFactoryURL != "https://searaboom.goossen.dev" {
		t.Fatalf("default URL %s", DefaultFactoryURL)
	}
	if strings.Contains(DefaultFactoryURL, "0.5.20") {
		t.Fatal("default URL must not pin a firmware version")
	}
}

func TestValidateAndRefreshPicksLatest(t *testing.T) {
	dest := t.TempDir()
	blobs := map[string][]byte{}
	meta := Manifest{
		Ready:   true,
		Version: "9.9.9",
		Files:   nil,
	}
	for _, item := range layout.Files {
		meta.Files = append(meta.Files, FileMeta{Filename: item.Filename, Offset: int(item.Offset)})
		blobs["https://example.test/api/factory/"+item.Filename] = []byte("bin-" + item.Key)
	}
	raw, _ := json.Marshal(meta)
	blobs["https://example.test/api/factory"] = raw

	src := NewLive("https://example.test", dest, func(url string) ([]byte, error) {
		b, ok := blobs[url]
		if !ok {
			t.Fatalf("unexpected url %s", url)
		}
		return b, nil
	})
	if err := src.Refresh(); err != nil {
		t.Fatal(err)
	}
	if src.Version != "9.9.9" {
		t.Fatalf("version %s", src.Version)
	}
	if filepath.Base(src.Dir) != "9.9.9" {
		t.Fatalf("dir %s", src.Dir)
	}
	got, err := os.ReadFile(filepath.Join(src.Dir, "app.bin"))
	if err != nil || string(got) != "bin-app" {
		t.Fatalf("app.bin %q %v", got, err)
	}

	meta.Version = "9.9.10"
	raw, _ = json.Marshal(meta)
	blobs["https://example.test/api/factory"] = raw
	for _, item := range layout.Files {
		blobs["https://example.test/api/factory/"+item.Filename] = []byte("new-" + item.Key)
	}
	if err := src.Refresh(); err != nil {
		t.Fatal(err)
	}
	if src.Version != "9.9.10" {
		t.Fatalf("did not pick newer live version: %s", src.Version)
	}
	got, _ = os.ReadFile(filepath.Join(src.Dir, "app.bin"))
	if string(got) != "new-app" {
		t.Fatalf("expected newly downloaded app, got %q", got)
	}
}

func TestValidateOffsetGuard(t *testing.T) {
	meta := Manifest{Ready: true, Version: "1.2.3"}
	for _, item := range layout.Files {
		off := int(item.Offset)
		if item.Filename == "app.bin" {
			off = 1
		}
		meta.Files = append(meta.Files, FileMeta{Filename: item.Filename, Offset: off})
	}
	err := Validate(meta)
	if err == nil || !strings.Contains(err.Error(), "app.bin") {
		t.Fatalf("expected app.bin offset error, got %v", err)
	}
}
