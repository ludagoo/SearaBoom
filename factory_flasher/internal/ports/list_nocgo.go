//go:build !linux && !windows

package ports

import (
	"os/exec"
	"path/filepath"
	"strings"

	"go.bug.st/serial"
)

// ioregUSBDump is `ioreg -p IOUSB -l -w0` so CGO stays off on darwin.
var ioregUSBDump = func() ([]byte, error) {
	return exec.Command("ioreg", "-p", "IOUSB", "-l", "-w0").Output()
}

// Darwin (and other non-Linux/Windows) builds avoid go.bug.st/serial/enumerator
// so CGO_ENABLED=0 cross-compiles from Linux. Match common ESP USB names.
// Only the cu.* callout node is kept; tty.* is the same ESP.
func List() ([]Port, error) {
	names, err := serial.GetPortsList()
	if err != nil {
		names = nil
	}
	seen := map[string]struct{}{}
	usb := map[string]ioregUSB{}
	if dump, err := ioregUSBDump(); err == nil {
		usb = ParseIORegCalloutSerials(dump)
	}
	var out []Port
	add := func(name string) {
		if name == "" || IsBSDDialin(name) {
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
		if info, ok := usb[name]; ok {
			applyIORegUSB(&p, info)
		} else if info, ok := usb[filepath.Base(name)]; ok {
			applyIORegUSB(&p, info)
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
	} {
		matches, _ := filepath.Glob(pat)
		for _, name := range matches {
			add(name)
		}
	}
	return CollapseBSDTwins(out), nil
}

func existsNamedPort(device string) bool {
	if IsBSDDialin(device) {
		// Callout node is what we flash; dialin disappearing is not "gone".
		device = strings.Replace(device, "/dev/tty.", "/dev/cu.", 1)
		if !strings.HasPrefix(device, "/dev/") {
			base := filepath.Base(device)
			if strings.HasPrefix(base, "tty.") {
				device = "/dev/cu." + base[len("tty."):]
			}
		}
	}
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
