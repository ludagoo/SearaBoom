package session

import (
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/ports"
)

// Demo returns a session that walks the operator loop with no USB writes
// and no live firmware download. Image version is 0.0.0 (not a live pin).
func Demo() *Session {
	portsFn := func() ([]ports.Port, error) {
		return []ports.Port{{
			Device:       "/dev/ttyACM-demo",
			Serial:       "DEMOBOX",
			VID:          0x303A,
			PID:          0x1001,
			Manufacturer: "Espressif",
			Product:      "USB JTAG/serial debug unit",
			HWID:         "USB VID:PID=303A:1001 SER=DEMOBOX",
		}}, nil
	}
	flashFn := func(port, imageDir string, onLine func(string)) error {
		for _, line := range []string{
			"Chip is ESP32-S3",
			"Configuring flash size...",
			"Writing at 0x00000000... (12 %)",
			"Writing at 0x00008000... (28 %)",
			"Writing at 0x0000f000... (41 %)",
			"Writing at 0x00020000... (67 %)",
			"Writing at 0x003a0000... (100 %)",
			"Hash of data verified.",
			"Hard resetting via RTS pin...",
		} {
			onLine(line)
			time.Sleep(40 * time.Millisecond)
		}
		return nil
	}
	serialFn := func(port, command string, wait time.Duration) string {
		switch command {
		case "ver":
			return "version=0.0.0 kconfig=0.0.0 board=s3-zero\n"
		case "board":
			return "board=s3-zero i2s dout=6 bclk=7 ws=8 vol+=3 vol-=2 led=21+48 rise21=0 rise47=0 rise48=0 pulled_low=none\n"
		case "touch cal":
			time.Sleep(40 * time.Millisecond)
			return "touch cal: hold + then -\ncal: hands off\ncal: hold +\ncal: + peak=0.180\ncal: hold -\ncal: - peak=0.175\ncal done vol+=0.144 vol-=0.140\ntouch cal ESP_OK\n"
		default:
			return ""
		}
	}
	return New(Options{
		ImageDir:  "/tmp/searaboom-factory-demo",
		ImageVer:  "0.0.0",
		Source:    "demo (no live download)",
		ListPorts: portsFn,
		Flash:     flashFn,
		Serial:    serialFn,
		WaitPort:  func(string, time.Duration) bool { return true },
		QAPaths:   []string{},
		Sleep:     func(time.Duration) {},
	})
}
