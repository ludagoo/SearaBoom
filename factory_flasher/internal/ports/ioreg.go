package ports

import (
	"path/filepath"
	"regexp"
	"strings"
)

var (
	ioregNodeRE    = regexp.MustCompile(`^([ |+\-]*)\+-o `)
	ioregSerialRE  = regexp.MustCompile(`"(?:USB Serial Number|kUSBSerialNumberString)"\s*=\s*"([^"]+)"`)
	ioregCalloutRE = regexp.MustCompile(`"IOCalloutDevice"\s*=\s*"([^"]+)"`)
	ioregDialinRE  = regexp.MustCompile(`"IODialinDevice"\s*=\s*"([^"]+)"`)
	ioregVendorRE  = regexp.MustCompile(`"idVendor"\s*=\s*(\d+)`)
	ioregProductRE = regexp.MustCompile(`"idProduct"\s*=\s*(\d+)`)
)

type ioregUSB struct {
	Serial string
	VID    int
	PID    int
}

// ParseIORegCalloutSerials maps Darwin IOCalloutDevice / IODialinDevice
// paths to USB serial numbers from `ioreg -p IOUSB -l -w0`.
func ParseIORegCalloutSerials(dump []byte) map[string]ioregUSB {
	out := map[string]ioregUSB{}
	type frame struct {
		indent int
		usb    ioregUSB
	}
	stack := []frame{{indent: -1}}
	assign := func(path string) {
		if path == "" {
			return
		}
		usb := stack[len(stack)-1].usb
		for i := len(stack) - 1; i >= 0 && usb.Serial == "" && usb.VID == 0; i-- {
			usb = stack[i].usb
		}
		if usb.Serial == "" && usb.VID == 0 {
			return
		}
		out[path] = usb
		out[filepath.Base(path)] = usb
	}
	for _, line := range strings.Split(string(dump), "\n") {
		if m := ioregNodeRE.FindStringSubmatch(line); m != nil {
			indent := len(m[1])
			for len(stack) > 1 && stack[len(stack)-1].indent >= indent {
				stack = stack[:len(stack)-1]
			}
			cur := stack[len(stack)-1].usb
			stack = append(stack, frame{indent: indent, usb: cur})
			continue
		}
		if len(stack) == 0 {
			continue
		}
		top := &stack[len(stack)-1]
		if m := ioregSerialRE.FindStringSubmatch(line); m != nil {
			top.usb.Serial = m[1]
		}
		if m := ioregVendorRE.FindStringSubmatch(line); m != nil {
			top.usb.VID = atoiDec(m[1])
		}
		if m := ioregProductRE.FindStringSubmatch(line); m != nil {
			top.usb.PID = atoiDec(m[1])
		}
		if m := ioregCalloutRE.FindStringSubmatch(line); m != nil {
			assign(m[1])
		}
		if m := ioregDialinRE.FindStringSubmatch(line); m != nil {
			assign(m[1])
		}
	}
	return out
}

func atoiDec(s string) int {
	n := 0
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0
		}
		n = n*10 + int(c-'0')
	}
	return n
}

func applyIORegUSB(p *Port, info ioregUSB) {
	if info.Serial != "" && p.Serial == "" {
		p.Serial = info.Serial
	}
	if info.VID != 0 && p.VID == 0 {
		p.VID = info.VID
	}
	if info.PID != 0 && p.PID == 0 {
		p.PID = info.PID
	}
}
