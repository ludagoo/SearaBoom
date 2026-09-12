package flash

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/layout"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/ports"
	"tinygo.org/x/espflasher/pkg/espflasher"
)

type LineFunc func(string)

type lineLogger struct {
	fn LineFunc
}

func (l lineLogger) Logf(format string, args ...interface{}) {
	if l.fn == nil {
		return
	}
	l.fn(fmt.Sprintf(format, args...))
}

func ResetModeFor(port ports.Port) espflasher.ResetMode {
	switch port.VID {
	case 0x303A:
		return espflasher.ResetUSBJTAG
	case 0x10C4, 0x1A86:
		return espflasher.ResetDefault
	}
	name := strings.ToLower(port.Device)
	if strings.Contains(name, "acm") || strings.Contains(name, "usbmodem") {
		return espflasher.ResetUSBJTAG
	}
	// Unknown adapter: try classic DTR/RTS, then USB-JTAG (esptool-style).
	return espflasher.ResetAuto
}

func Port(device, imageDir string, hint ports.Port, onLine LineFunc) error {
	files, err := layout.FileMap(imageDir)
	if err != nil {
		return err
	}
	images := make([]espflasher.ImagePart, 0, len(layout.Files))
	for _, item := range layout.Files {
		data, err := os.ReadFile(files[item.Filename])
		if err != nil {
			return err
		}
		images = append(images, espflasher.ImagePart{Data: data, Offset: item.Offset})
	}

	opts := espflasher.DefaultOptions()
	opts.ChipType = espflasher.ChipESP32S3
	opts.FlashBaudRate = layout.Baud
	opts.FlashMode = layout.FlashMode
	opts.FlashFreq = layout.FlashFreq
	opts.FlashSize = layout.FlashSize
	opts.ResetMode = ResetModeFor(hint)
	opts.Logger = lineLogger{fn: onLine}
	if onLine != nil {
		onLine(fmt.Sprintf("flash %s chip=%s baud=%d before=%s after=%s reset=%s",
			device, layout.Chip, layout.Baud, layout.Before, layout.After, opts.ResetMode))
	}

	flasher, err := espflasher.New(device, opts)
	if err != nil {
		return fmt.Errorf("connect bootloader: %w", err)
	}
	defer flasher.Close()

	err = flasher.FlashImages(images, func(current, total int) {
		if onLine == nil {
			return
		}
		pct := 0
		if total > 0 {
			pct = current * 100 / total
			if pct > 100 {
				pct = 100
			}
		}
		onLine(fmt.Sprintf("Writing at 0x00000000... (%d %%)", pct))
	})
	if err != nil {
		return err
	}
	if onLine != nil {
		onLine("Hash of data verified.")
		onLine("Hard resetting via RTS pin...")
	}
	flasher.Reset()
	return nil
}

func Wait(device string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if ports.Exists(device) {
			return true
		}
		time.Sleep(200 * time.Millisecond)
	}
	return ports.Exists(device)
}
