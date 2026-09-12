// Package layout is the USB factory flash map.
//
// Must stay aligned with server/factory_layout.py, scripts/hw_restore.sh,
// and scripts/snapshot_factory.sh.
package layout

import (
	"fmt"
	"os"
	"path/filepath"
)

const (
	Chip      = "esp32s3"
	Baud      = 460800
	Before    = "default_reset"
	After     = "hard_reset"
	FlashMode = "dio"
	FlashFreq = "80m"
	FlashSize = "4MB"
)

// Espressif USB-JTAG/serial, Silicon Labs CP210x, WCH CH340.
var USBVendorIDs = []int{0x303A, 0x10C4, 0x1A86}

type File struct {
	Key      string
	Offset   uint32
	Filename string
	BuildRel string
}

var Files = []File{
	{Key: "bootloader", Offset: 0x0, Filename: "bootloader.bin", BuildRel: "bootloader/bootloader.bin"},
	{Key: "partitions", Offset: 0x8000, Filename: "partition-table.bin", BuildRel: "partition_table/partition-table.bin"},
	{Key: "otadata", Offset: 0xF000, Filename: "ota_data_initial.bin", BuildRel: "ota_data_initial.bin"},
	{Key: "app", Offset: 0x20000, Filename: "app.bin", BuildRel: "searaboom.bin"},
	{Key: "storage", Offset: 0x3A0000, Filename: "storage.bin", BuildRel: "storage.bin"},
}

func RequiredFilenames() []string {
	out := make([]string, len(Files))
	for i, f := range Files {
		out[i] = f.Filename
	}
	return out
}

func OffsetHex(offset uint32) string {
	return fmt.Sprintf("0x%x", offset)
}

func SlotReady(dir string) bool {
	for _, f := range Files {
		st, err := os.Stat(filepath.Join(dir, f.Filename))
		if err != nil || st.IsDir() {
			return false
		}
	}
	return true
}

func FileMap(dir string) (map[string]string, error) {
	out := make(map[string]string, len(Files))
	var missing []string
	for _, f := range Files {
		path := filepath.Join(dir, f.Filename)
		st, err := os.Stat(path)
		if err != nil || st.IsDir() {
			missing = append(missing, f.Filename)
			continue
		}
		out[f.Filename] = path
	}
	if len(missing) > 0 {
		return nil, fmt.Errorf("missing factory files: %s", joinComma(missing))
	}
	return out, nil
}

func joinComma(parts []string) string {
	out := ""
	for i, p := range parts {
		if i > 0 {
			out += ", "
		}
		out += p
	}
	return out
}
