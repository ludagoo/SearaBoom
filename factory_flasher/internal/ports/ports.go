package ports

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/layout"
)

type Port struct {
	Device       string
	Serial       string
	VID          int
	PID          int
	Manufacturer string
	Product      string
	HWID         string
}

func (p Port) Identity() string {
	if p.Serial != "" {
		return p.Serial
	}
	if p.HWID != "" {
		return p.HWID
	}
	return p.Device
}

type Lister func() ([]Port, error)

func NormUSBSerial(serial string) string {
	s := strings.ToLower(serial)
	s = strings.ReplaceAll(s, ":", "")
	s = strings.ReplaceAll(s, "-", "")
	return s
}

func QAConfigPath() string {
	if env := os.Getenv("SEARABOOM_QA_BOXES"); env != "" {
		return env
	}
	cfg, err := os.UserConfigDir()
	if err != nil || cfg == "" {
		home, _ := os.UserHomeDir()
		cfg = filepath.Join(home, ".config")
	}
	return filepath.Join(cfg, "searaboom", "qa-boxes.json")
}

func QAUSBSerials(path string) map[string]struct{} {
	found := map[string]struct{}{}
	if path == "" {
		path = QAConfigPath()
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return found
	}
	var data struct {
		Boxes map[string]struct {
			USBSerial string `json:"usb_serial"`
		} `json:"boxes"`
	}
	if err := json.Unmarshal(raw, &data); err != nil {
		return found
	}
	for _, rec := range data.Boxes {
		if s := strings.TrimSpace(rec.USBSerial); s != "" {
			found[NormUSBSerial(s)] = struct{}{}
		}
	}
	return found
}

func IsQADevicePath(device string) bool {
	text := device
	real := device
	if abs, err := filepath.EvalSymlinks(device); err == nil {
		real = abs
	}
	return strings.Contains(text, "searaboom-qa") || strings.Contains(real, "searaboom-qa")
}

func QADevicePaths(configPath string) map[string]struct{} {
	found := map[string]struct{}{}
	add := func(p string) {
		if p == "" {
			return
		}
		found[p] = struct{}{}
		if abs, err := filepath.EvalSymlinks(p); err == nil {
			found[abs] = struct{}{}
		}
	}
	matches, _ := filepath.Glob("/dev/searaboom-qa-*")
	for _, p := range matches {
		add(p)
	}
	if configPath == "" {
		configPath = QAConfigPath()
	}
	raw, err := os.ReadFile(configPath)
	if err != nil {
		return found
	}
	var data struct {
		Boxes map[string]struct {
			USBSerial string `json:"usb_serial"`
		} `json:"boxes"`
	}
	if err := json.Unmarshal(raw, &data); err != nil {
		return found
	}
	for _, rec := range data.Boxes {
		serial := strings.TrimSpace(rec.USBSerial)
		if serial == "" {
			continue
		}
		byID := filepath.Join("/dev/serial/by-id", "usb-Espressif_USB_JTAG_serial_debug_unit_"+serial+"-if00")
		if _, err := os.Stat(byID); err == nil {
			add(byID)
		}
	}
	return found
}

func IsSearaboom(p Port) bool {
	for _, vid := range layout.USBVendorIDs {
		if p.VID == vid {
			return true
		}
	}
	blob := strings.ToLower(p.Manufacturer + " " + p.Product + " " + p.HWID + " " + p.Device)
	return strings.Contains(blob, "espressif") ||
		strings.Contains(blob, "usb jtag") ||
		strings.Contains(blob, "usbmodem") ||
		strings.Contains(blob, "usbserial") ||
		strings.Contains(blob, "wchusbserial") ||
		strings.Contains(blob, "slab_usbtouart") ||
		strings.Contains(blob, "cp210") ||
		strings.Contains(blob, "ch340")
}

func parseHexID(s string) int {
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(strings.ToLower(s), "0x")
	if s == "" {
		return 0
	}
	n, err := strconv.ParseInt(s, 16, 32)
	if err != nil {
		return 0
	}
	return int(n)
}

func Eligible(in []Port, qaPaths []string, qaSerials []string) []Port {
	blocked := map[string]struct{}{}
	blockedSerials := map[string]struct{}{}
	if qaPaths == nil {
		for p := range QADevicePaths("") {
			blocked[p] = struct{}{}
		}
		blockedSerials = QAUSBSerials("")
	} else {
		for _, raw := range qaPaths {
			blocked[raw] = struct{}{}
			if abs, err := filepath.EvalSymlinks(raw); err == nil {
				blocked[abs] = struct{}{}
			}
		}
		for _, s := range qaSerials {
			if s != "" {
				blockedSerials[NormUSBSerial(s)] = struct{}{}
			}
		}
	}
	var out []Port
	for _, p := range in {
		if IsQADevicePath(p.Device) {
			continue
		}
		real := p.Device
		if abs, err := filepath.EvalSymlinks(p.Device); err == nil {
			real = abs
		}
		if _, ok := blocked[p.Device]; ok {
			continue
		}
		if _, ok := blocked[real]; ok {
			continue
		}
		if p.Serial != "" {
			if _, ok := blockedSerials[NormUSBSerial(p.Serial)]; ok {
				continue
			}
		}
		out = append(out, p)
	}
	return out
}

func NewAmong(current []Port, baseline map[string]struct{}) []Port {
	var out []Port
	for _, p := range current {
		if _, ok := baseline[p.Identity()]; !ok {
			out = append(out, p)
		}
	}
	return out
}

func Exists(device string) bool {
	if device == "" {
		return false
	}
	if _, err := os.Stat(device); err == nil {
		return true
	}
	return existsNamedPort(device)
}
