package session

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/image"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/layout"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/ports"
)

func box(device, serial string) ports.Port {
	return ports.Port{
		Device: device, Serial: serial, VID: 0x303A, PID: 0x1001,
		Manufacturer: "Espressif", Product: "USB JTAG/serial debug unit",
		HWID: "USB VID:PID=303A:1001 SER=" + serial,
	}
}

func okSerial(port, command string, wait time.Duration) string {
	switch command {
	case "ver":
		return "version=9.9.9 kconfig=9.9.9 board=s3-zero\n"
	case "board":
		return "board=s3-zero i2s dout=6\n"
	case "touch cal":
		return "cal: hold +\ncal: hold -\ncal done\ntouch cal ESP_OK\n"
	default:
		return ""
	}
}

func TestDisarmedDoesNotFlash(t *testing.T) {
	flashed := 0
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return []ports.Port{box("/dev/ttyACM5", "NEW")}, nil },
		Flash:     func(string, string, func(string)) error { flashed++; return nil },
		Serial:    func(string, string, time.Duration) string { return "" },
		WaitPort:  func(string, time.Duration) bool { return true },
		QAPaths:   []string{},
	})
	sess.Tick()
	if flashed != 0 || sess.Snapshot().Phase != "idle" {
		t.Fatalf("flashed=%d phase=%s", flashed, sess.Snapshot().Phase)
	}
}

func TestArmAndFlashPass(t *testing.T) {
	var flashed []string
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return []ports.Port{box("/dev/ttyACM5", "BOX1")}, nil },
		Flash: func(port, _ string, onLine func(string)) error {
			flashed = append(flashed, port)
			onLine("Writing at 0x00020000... (50 %)")
			onLine("Writing at 0x00020000... (100 %)")
			return nil
		},
		Serial:   okSerial,
		WaitPort: func(string, time.Duration) bool { return true },
		QAPaths:  []string{},
		Sleep:    func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	snap := sess.Snapshot()
	if len(flashed) != 1 || flashed[0] != "/dev/ttyACM5" {
		t.Fatalf("flashed %v", flashed)
	}
	if snap.Phase != "pass" || snap.Progress != 100 || snap.BoxesDone != 1 {
		t.Fatalf("%+v", snap)
	}
	for _, s := range snap.Confirm {
		if s.Status != "pass" {
			t.Fatalf("step %+v", s)
		}
	}
}

func TestFlashFail(t *testing.T) {
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return []ports.Port{box("/dev/ttyACM5", "BOX2")}, nil },
		Flash:     func(string, string, func(string)) error { return errors.New("esptool exited 1") },
		Serial:    func(string, string, time.Duration) string { return "" },
		WaitPort:  func(string, time.Duration) bool { return true },
		QAPaths:   []string{},
		Sleep:     func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	snap := sess.Snapshot()
	if snap.Phase != "fail" {
		t.Fatalf("phase %s", snap.Phase)
	}
	if snap.LastError == "" && !strings.Contains(strings.ToLower(snap.Message), "esptool") && !strings.Contains(strings.ToLower(snap.Message), "flash") {
		t.Fatalf("expected flash error %+v", snap)
	}
}

func TestSkipQA(t *testing.T) {
	flashed := 0
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return []ports.Port{box("/dev/searaboom-qa-zero-fast", "QA")}, nil },
		Flash:     func(string, string, func(string)) error { flashed++; return nil },
		Serial:    func(string, string, time.Duration) string { return "" },
		WaitPort:  func(string, time.Duration) bool { return true },
		QAPaths:   []string{"/dev/searaboom-qa-zero-fast"},
		Sleep:     func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	if flashed != 0 || sess.Snapshot().Phase != "watching" {
		t.Fatalf("flashed=%d phase=%s", flashed, sess.Snapshot().Phase)
	}
}

func TestCalTimeoutDoesNotSendSecondTouchCal(t *testing.T) {
	var cmds []string
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return []ports.Port{box("/dev/ttyACM5", "BOX3")}, nil },
		Flash:     func(string, string, func(string)) error { return nil },
		Serial: func(_ string, command string, _ time.Duration) string {
			cmds = append(cmds, command)
			switch command {
			case "ver":
				return "version=9.9.9 kconfig=9.9.9 board=s3-zero\n"
			case "board":
				return "board=s3-zero i2s dout=6\n"
			case "touch cal":
				return "cal: hold +\n"
			default:
				return ""
			}
		},
		WaitPort: func(string, time.Duration) bool { return true },
		QAPaths:  []string{},
		Sleep:    func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	n := 0
	for _, c := range cmds {
		if c == "touch cal" {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("touch cal count %d (%v)", n, cmds)
	}
	if sess.Snapshot().Phase != "fail" || sess.Snapshot().LastError != "cal timeout" {
		t.Fatalf("%+v", sess.Snapshot())
	}
}

func TestVerRetriesOnce(t *testing.T) {
	n := 0
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return []ports.Port{box("/dev/ttyACM5", "BOX4")}, nil },
		Flash:     func(string, string, func(string)) error { return nil },
		Serial: func(_ string, command string, _ time.Duration) string {
			if command == "ver" {
				n++
				if n == 1 {
					return ""
				}
				return "version=9.9.9 kconfig=9.9.9 board=s3-zero\n"
			}
			if command == "board" {
				return "board=s3-zero i2s dout=6\n"
			}
			if command == "touch cal" {
				return "cal done\ntouch cal ESP_OK\n"
			}
			return ""
		},
		WaitPort: func(string, time.Duration) bool { return true },
		QAPaths:  []string{},
		Sleep:    func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	if n != 2 || sess.Snapshot().Phase != "pass" {
		t.Fatalf("ver=%d phase=%s", n, sess.Snapshot().Phase)
	}
}

func TestArmRefreshesLiveImage(t *testing.T) {
	version := "1.0.0"
	gets := 0
	get := func(url string) ([]byte, error) {
		gets++
		if strings.HasSuffix(url, "/api/factory") {
			files := ""
			for i, item := range layout.Files {
				if i > 0 {
					files += ","
				}
				files += `{"filename":"` + item.Filename + `","offset":` + itoa(int(item.Offset)) + `}`
			}
			return []byte(`{"ready":true,"version":"` + version + `","files":[` + files + `]}`), nil
		}
		return []byte("bin"), nil
	}
	live := image.NewLive("https://example.test", t.TempDir(), get)
	sess := New(Options{Live: live, ListPorts: func() ([]ports.Port, error) { return nil, nil }, QAPaths: []string{}})
	if err := sess.RefreshImage(); err != nil {
		t.Fatal(err)
	}
	if sess.Snapshot().ImageVersion != "1.0.0" {
		t.Fatal(sess.Snapshot().ImageVersion)
	}
	version = "1.0.1"
	sess.Arm(true)
	if sess.Snapshot().ImageVersion != "1.0.1" {
		t.Fatalf("ARM did not pick live version: %s", sess.Snapshot().ImageVersion)
	}
	if gets < 2 {
		t.Fatalf("expected catalog refresh on ARM, gets=%d", gets)
	}
}

func TestArmOneESPCalloutAndDialinDoesNotAskUnplug(t *testing.T) {
	twins := []ports.Port{
		{Device: "/dev/cu.usbmodem1101", VID: 0x303A, Product: "USB JTAG/serial debug unit"},
		{Device: "/dev/tty.usbmodem1101", VID: 0x303A, Product: "USB JTAG/serial debug unit"},
	}
	sess := New(Options{
		ImageDir:  "/tmp",
		ImageVer:  "9.9.9",
		ListPorts: func() ([]ports.Port, error) { return twins, nil },
		Flash:     func(string, string, func(string)) error { return nil },
		Serial:    okSerial,
		WaitPort:  func(string, time.Duration) bool { return true },
		QAPaths:   []string{},
		Sleep:     func(time.Duration) {},
	})
	snap := sess.Arm(true)
	if strings.Contains(snap.Message, "Unplug extra") {
		t.Fatalf("one ESP listed as two boxes: %s", snap.Message)
	}
	if !strings.Contains(snap.Message, "Flashing the box on /dev/cu.usbmodem1101") {
		t.Fatalf("want plug-then-arm flash, got %s", snap.Message)
	}
	sess.Tick()
	if sess.Snapshot().Phase != "pass" {
		t.Fatalf("phase %s", sess.Snapshot().Phase)
	}
}

func TestSecondBoxSameCalloutPathStillFlashes(t *testing.T) {
	device := "/dev/cu.usbmodem1101"
	present := true
	var flashed []string
	sess := New(Options{
		ImageDir: "/tmp",
		ImageVer: "9.9.9",
		ListPorts: func() ([]ports.Port, error) {
			if !present {
				return nil, nil
			}
			return []ports.Port{{
				Device: device, VID: 0x303A,
				Product: "USB JTAG/serial debug unit",
			}}, nil
		},
		Flash: func(port, _ string, onLine func(string)) error {
			flashed = append(flashed, port)
			return nil
		},
		Serial:   okSerial,
		WaitPort: func(string, time.Duration) bool { return true },
		QAPaths:  []string{},
		Sleep:    func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	if len(flashed) != 1 || sess.Snapshot().Phase != "pass" {
		t.Fatalf("first %v phase=%s", flashed, sess.Snapshot().Phase)
	}
	present = false
	sess.Tick()
	if sess.Snapshot().Phase != "watching" {
		t.Fatalf("after unplug %s", sess.Snapshot().Phase)
	}
	present = true
	sess.Tick()
	if len(flashed) != 2 || sess.Snapshot().Phase != "pass" {
		t.Fatalf("second chassis on same cu path skipped: flashed=%v phase=%s", flashed, sess.Snapshot().Phase)
	}
}

func TestDoneIDsSkipSameUSBSerialNotSamePath(t *testing.T) {
	device := "/dev/cu.usbmodem1101"
	serial := "CHIP-A"
	present := true
	flashed := 0
	sess := New(Options{
		ImageDir: "/tmp",
		ImageVer: "9.9.9",
		ListPorts: func() ([]ports.Port, error) {
			if !present {
				return nil, nil
			}
			return []ports.Port{{
				Device: device, Serial: serial, VID: 0x303A,
				Product: "USB JTAG/serial debug unit",
			}}, nil
		},
		Flash:    func(string, string, func(string)) error { flashed++; return nil },
		Serial:   okSerial,
		WaitPort: func(string, time.Duration) bool { return true },
		QAPaths:  []string{},
		Sleep:    func(time.Duration) {},
	})
	sess.Arm(true)
	sess.Tick()
	present = false
	sess.Tick()
	present = true
	sess.Tick()
	if flashed != 1 {
		t.Fatalf("same USB serial should stay done, flashed=%d", flashed)
	}
	serial = "CHIP-B"
	sess.Tick()
	if flashed != 2 {
		t.Fatalf("new chassis on same path must flash, flashed=%d", flashed)
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	s := ""
	for n > 0 {
		s = string(rune('0'+n%10)) + s
		n /= 10
	}
	return s
}
