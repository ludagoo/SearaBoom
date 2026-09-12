package ports

import "testing"

func TestQAAndEligible(t *testing.T) {
	if !IsQADevicePath("/dev/searaboom-qa-zero-fast") {
		t.Fatal("qa path")
	}
	ports := []Port{
		{Device: "/dev/searaboom-qa-zero-fast", Serial: "AAA", VID: 0x303A, Product: "USB JTAG"},
		{Device: "/dev/ttyACM3", Serial: "BBB", VID: 0x303A, Product: "USB JTAG"},
	}
	ok := Eligible(ports, []string{"/dev/searaboom-qa-zero-fast"}, nil)
	if len(ok) != 1 || ok[0].Serial != "BBB" {
		t.Fatalf("eligible %+v", ok)
	}
}

func TestQASerialSkipped(t *testing.T) {
	plist := []Port{
		{Device: "/dev/ttyACM3", Serial: "D0:CF:13:07:DE:FC", VID: 0x303A, Product: "USB JTAG"},
		{Device: "/dev/ttyACM4", Serial: "BOX", VID: 0x303A, Product: "USB JTAG"},
	}
	ok := Eligible(plist, []string{}, []string{"D0:CF:13:07:DE:FC"})
	if len(ok) != 1 || ok[0].Serial != "BOX" {
		t.Fatalf("eligible %+v", ok)
	}
}

func TestNewAmong(t *testing.T) {
	a := Port{Device: "/dev/ttyACM0", Serial: "A", VID: 0x303A}
	b := Port{Device: "/dev/ttyACM1", Serial: "B", VID: 0x303A}
	got := NewAmong([]Port{a, b}, map[string]struct{}{"A": {}})
	if len(got) != 1 || got[0].Serial != "B" {
		t.Fatalf("%+v", got)
	}
}

func TestIsSearaboomVID(t *testing.T) {
	if !IsSearaboom(Port{Device: "/dev/ttyACM0", VID: 0x303A, Product: "USB JTAG"}) {
		t.Fatal("303A")
	}
	if !IsSearaboom(Port{Device: "/dev/cu.usbmodem1101"}) {
		t.Fatal("darwin usbmodem")
	}
	if IsSearaboom(Port{Device: "/dev/ttyUSB9", VID: 0x1234, Product: "Hub", Manufacturer: "Other", HWID: "USB VID:PID=1234:0001"}) {
		t.Fatal("filtered")
	}
}

func TestNormUSBSerial(t *testing.T) {
	if NormUSBSerial("D0:CF:13:07:DE:FC") != "d0cf1307defc" {
		t.Fatal(NormUSBSerial("D0:CF:13:07:DE:FC"))
	}
}

func TestOneESPCalloutAndDialinIsOneIdentity(t *testing.T) {
	twins := []Port{
		{Device: "/dev/cu.usbmodem1101", VID: 0x303A, Product: "USB JTAG"},
		{Device: "/dev/tty.usbmodem1101", VID: 0x303A, Product: "USB JTAG"},
	}
	got := CollapseBSDTwins(twins)
	if len(got) != 1 {
		t.Fatalf("want 1 port, got %+v", got)
	}
	if got[0].Device != "/dev/cu.usbmodem1101" {
		t.Fatalf("prefer cu: %+v", got[0])
	}
	if got[0].Identity() != "/dev/cu.usbmodem1101" {
		t.Fatalf("path-only identity %s", got[0].Identity())
	}
	if got[0].HasStableIdentity() {
		t.Fatal("empty serial is not a stable identity")
	}

	twins[0].Serial = "54:32:04:AA:BB:CC"
	got = CollapseBSDTwins(twins)
	if len(got) != 1 || got[0].Identity() != "54:32:04:AA:BB:CC" {
		t.Fatalf("usb serial identity %+v", got)
	}
	ok := Eligible(twins, []string{}, nil)
	if len(ok) != 1 || ok[0].Device != "/dev/cu.usbmodem1101" {
		t.Fatalf("eligible twins %+v", ok)
	}
}

func TestIdentityDoesNotUsePathAsStable(t *testing.T) {
	p := Port{Device: "/dev/cu.usbmodem1101", HWID: "/dev/cu.usbmodem1101", VID: 0x303A}
	if p.HasStableIdentity() {
		t.Fatal("hwid path is not stable")
	}
	if p.Identity() != "/dev/cu.usbmodem1101" {
		t.Fatalf("identity %s", p.Identity())
	}
	p.Serial = "CHIP1"
	if !p.HasStableIdentity() || p.Identity() != "CHIP1" {
		t.Fatalf("serial identity %+v", p)
	}
}
