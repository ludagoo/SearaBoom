package ports

import "testing"

const ioregOneESP = `
+-o Root  <class IORegistryEntry>
  +-o AppleUSBXHCI
    +-o USB JTAG/serial debug unit@01100000  <class IOUSBHostDevice>
      | {
      |   "idVendor" = 12346
      |   "idProduct" = 4097
      |   "USB Serial Number" = "54:32:04:AA:BB:CC"
      |   "USB Vendor Name" = "Espressif"
      | }
      +-o AppleUSBACMData
        +-o IOSerialBSDClient
          | {
          |   "IOCalloutDevice" = "/dev/cu.usbmodem1101"
          |   "IODialinDevice" = "/dev/tty.usbmodem1101"
          | }
`

func TestParseIORegOneESPBothBSDNames(t *testing.T) {
	got := ParseIORegCalloutSerials([]byte(ioregOneESP))
	cu := got["/dev/cu.usbmodem1101"]
	tty := got["/dev/tty.usbmodem1101"]
	if cu.Serial != "54:32:04:AA:BB:CC" {
		t.Fatalf("cu serial %+v", cu)
	}
	if tty.Serial != cu.Serial {
		t.Fatalf("tty serial %+v", tty)
	}
	if cu.VID != 0x303A {
		t.Fatalf("vid %d", cu.VID)
	}
}

func TestParseIORegTwoBoxes(t *testing.T) {
	dump := `
+-o Root
  +-o BoxA  <class IOUSBHostDevice>
    | { "USB Serial Number" = "AAAA" "idVendor" = 12346 }
    +-o IOSerialBSDClient
      | { "IOCalloutDevice" = "/dev/cu.usbmodemA" }
  +-o BoxB  <class IOUSBHostDevice>
    | { "USB Serial Number" = "BBBB" "idVendor" = 12346 }
    +-o IOSerialBSDClient
      | { "IOCalloutDevice" = "/dev/cu.usbmodemB" }
`
	got := ParseIORegCalloutSerials([]byte(dump))
	if got["/dev/cu.usbmodemA"].Serial != "AAAA" {
		t.Fatalf("A %+v", got["/dev/cu.usbmodemA"])
	}
	if got["/dev/cu.usbmodemB"].Serial != "BBBB" {
		t.Fatalf("B %+v", got["/dev/cu.usbmodemB"])
	}
}
