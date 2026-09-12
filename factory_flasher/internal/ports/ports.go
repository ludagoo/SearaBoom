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
	if p.HasStableIdentity() {
		return p.Serial
	}
	return p.Device
}

// HasStableIdentity is true when Serial is a USB serial number, not a
// callout/dialin path. Path-only ids must not go in session doneIDs:
// the next chassis on the same cable often keeps the same /dev name.
func (p Port) HasStableIdentity() bool {
	s := strings.TrimSpace(p.Serial)
	if s == "" {
		return false
	}
	return !looksLikeDevicePath(s)
}

func looksLikeDevicePath(s string) bool {
	low := strings.ToLower(s)
	return strings.HasPrefix(low, "/dev/") ||
		strings.HasPrefix(low, "cu.") ||
		strings.HasPrefix(low, "tty.") ||
		strings.HasPrefix(strings.ToUpper(s), "COM")
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

// IsBSDDialin is the macOS /dev/tty.* twin of /dev/cu.* (same ESP).
func IsBSDDialin(device string) bool {
	_, dialin, ok := bsdTwinKey(device)
	return ok && dialin
}

func bsdTwinKey(device string) (key string, dialin bool, ok bool) {
	base := filepath.Base(device)
	switch {
	case strings.HasPrefix(base, "tty."):
		return base[len("tty."):], true, true
	case strings.HasPrefix(base, "cu."):
		return base[len("cu."):], false, true
	default:
		return "", false, false
	}
}

// CollapseBSDTwins keeps one node per ESP on macOS: prefer cu.*, drop tty.*.
func CollapseBSDTwins(in []Port) []Port {
	type pair struct {
		callout *Port
		dialin  *Port
	}
	order := make([]string, 0, len(in))
	groups := map[string]*pair{}
	var others []Port
	for _, raw := range in {
		p := raw
		key, dialin, isTwin := bsdTwinKey(p.Device)
		if !isTwin {
			others = append(others, p)
			continue
		}
		g, ok := groups[key]
		if !ok {
			g = &pair{}
			groups[key] = g
			order = append(order, key)
		}
		if dialin {
			g.dialin = &p
		} else {
			g.callout = &p
		}
	}
	out := make([]Port, 0, len(order)+len(others))
	for _, key := range order {
		g := groups[key]
		chosen := g.callout
		if chosen == nil {
			chosen = g.dialin
		}
		if chosen != nil && g.dialin != nil && chosen != g.dialin {
			mergePort(chosen, *g.dialin)
		}
		if chosen != nil {
			out = append(out, *chosen)
		}
	}
	return append(out, others...)
}

func mergePort(dst *Port, src Port) {
	if dst.Serial == "" {
		dst.Serial = src.Serial
	}
	if dst.VID == 0 {
		dst.VID = src.VID
	}
	if dst.PID == 0 {
		dst.PID = src.PID
	}
	if dst.Manufacturer == "" {
		dst.Manufacturer = src.Manufacturer
	}
	if dst.Product == "" {
		dst.Product = src.Product
	}
}

func Eligible(in []Port, qaPaths []string, qaSerials []string) []Port {
	in = CollapseBSDTwins(in)
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
