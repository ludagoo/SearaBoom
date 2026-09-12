package layout

import "testing"

func TestOffsets(t *testing.T) {
	want := map[string]uint32{
		"bootloader.bin":       0x0,
		"partition-table.bin":  0x8000,
		"ota_data_initial.bin": 0xF000,
		"app.bin":              0x20000,
		"storage.bin":          0x3A0000,
	}
	if OffsetHex(0x20000) != "0x20000" {
		t.Fatalf("hex %s", OffsetHex(0x20000))
	}
	if OffsetHex(0) != "0x0" {
		t.Fatalf("hex0 %s", OffsetHex(0))
	}
	got := map[string]uint32{}
	for _, f := range Files {
		got[f.Filename] = f.Offset
	}
	for name, off := range want {
		if got[name] != off {
			t.Fatalf("%s offset %x want %x", name, got[name], off)
		}
	}
}

func TestFlashParams(t *testing.T) {
	if Chip != "esp32s3" || Baud != 460800 || Before != "default_reset" || After != "hard_reset" {
		t.Fatalf("chip/baud/reset mismatch")
	}
	if FlashMode != "dio" || FlashFreq != "80m" || FlashSize != "4MB" {
		t.Fatalf("flash params mismatch")
	}
}
