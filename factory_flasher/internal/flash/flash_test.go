package flash

import (
	"testing"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/ports"
	"tinygo.org/x/espflasher/pkg/espflasher"
)

func TestResetMode(t *testing.T) {
	if ResetModeFor(ports.Port{VID: 0x303A, Device: "/dev/ttyACM0"}) != espflasher.ResetUSBJTAG {
		t.Fatal("espressif usb-jtag")
	}
	if ResetModeFor(ports.Port{VID: 0x10C4, Device: "/dev/ttyUSB0"}) != espflasher.ResetDefault {
		t.Fatal("cp210x dtr/rts")
	}
	if ResetModeFor(ports.Port{VID: 0x1A86, Device: "/dev/ttyUSB1"}) != espflasher.ResetDefault {
		t.Fatal("ch340 dtr/rts")
	}
	if ResetModeFor(ports.Port{Device: "/dev/ttyACM2"}) != espflasher.ResetUSBJTAG {
		t.Fatal("acm name")
	}
}
