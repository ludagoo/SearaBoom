package serialcmd

import (
	"strings"
	"time"

	"go.bug.st/serial"
)

type LineFunc func(string)
type StopFunc func(acc string) bool
type NowFunc func() time.Time
type SleepFunc func(time.Duration)
type ReadChunkFunc func() string
type ExchangeFunc func(port, command string, wait time.Duration) string

func Collect(read ReadChunkFunc, now NowFunc, deadline time.Time, stop StopFunc, onLine LineFunc, sleep SleepFunc) string {
	if now == nil {
		now = time.Now
	}
	if sleep == nil {
		sleep = time.Sleep
	}
	acc := ""
	for now().Before(deadline) {
		chunk := ""
		if read != nil {
			chunk = read()
		}
		if chunk != "" {
			acc += chunk
			if onLine != nil {
				for _, line := range strings.Split(chunk, "\n") {
					line = strings.TrimRight(line, "\r")
					if line != "" {
						onLine(line)
					}
				}
			}
			if stop != nil && stop(acc) {
				break
			}
			continue
		}
		if !now().Before(deadline) {
			break
		}
		sleep(50 * time.Millisecond)
	}
	return acc
}

func Run(port, command string, wait time.Duration, onLine LineFunc, stop StopFunc, exchange ExchangeFunc) (string, error) {
	if exchange != nil {
		text := exchange(port, command, wait)
		if onLine != nil {
			for _, line := range strings.Split(text, "\n") {
				line = strings.TrimRight(line, "\r")
				if line != "" {
					onLine(line)
				}
			}
		}
		return text, nil
	}
	mode := &serial.Mode{BaudRate: 115200}
	ser, err := serial.Open(port, mode)
	if err != nil {
		return "", err
	}
	defer ser.Close()
	_ = ser.SetReadTimeout(400 * time.Millisecond)
	time.Sleep(200 * time.Millisecond)
	_ = ser.ResetInputBuffer()
	if _, err := ser.Write([]byte(command + "\n")); err != nil {
		return "", err
	}
	buf := make([]byte, 4096)
	text := Collect(func() string {
		n, err := ser.Read(buf)
		if err != nil || n <= 0 {
			return ""
		}
		return string(buf[:n])
	}, time.Now, time.Now().Add(wait), stop, onLine, func(time.Duration) {})
	return text, nil
}
