//go:build !linux && !windows

package ports

import (
	"path/filepath"
	"strings"

	"go.bug.st/serial"
)

// Darwin (and other non-Linux/Windows) builds avoid go.bug.st/serial/enumerator
// so CGO_ENABLED=0 cross-compiles from Linux. Match common ESP USB names.
func List() ([]Port, error) {
	names, err := serial.GetPortsList()
	if err != nil {
		names = nil
	}
	seen := map[string]struct{}{}
	var out []Port
	add := func(name string) {
		if name == "" {
			return
		}
		if _, ok := seen[name]; ok {
			return
		}
		seen[name] = struct{}{}
		p := Port{Device: name, HWID: name}
		low := strings.ToLower(name)
		if strings.Contains(low, "usbmodem") {
			p.VID = 0x303A
			p.Product = "USB JTAG/serial debug unit"
		}
		if IsSearaboom(p) {
			out = append(out, p)
		}
	}
	for _, name := range names {
		add(name)
	}
	for _, pat := range []string{
		"/dev/cu.usbmodem*",
		"/dev/cu.usbserial*",
		"/dev/cu.wchusbserial*",
		"/dev/cu.SLAB_USBtoUART*",
		"/dev/tty.usbmodem*",
		"/dev/tty.usbserial*",
	} {
		matches, _ := filepath.Glob(pat)
		for _, name := range matches {
			add(name)
		}
	}
	return out, nil
}

func existsNamedPort(device string) bool {
	names, err := serial.GetPortsList()
	if err != nil {
		return false
	}
	for _, name := range names {
		if name == device {
			return true
		}
	}
	return false
}
