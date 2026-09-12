package serialcmd

import (
	"testing"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/parse"
)

func TestCollectEarlyExitAndLateSuccess(t *testing.T) {
	now := time.Unix(0, 0)
	emitted := false
	clock := func() time.Time { return now }
	sleep := func(d time.Duration) { now = now.Add(d) }

	text := Collect(func() string {
		if now.Unix() >= 2 && !emitted {
			emitted = true
			return "cal: hold +\ncal done\ntouch cal ESP_OK\n"
		}
		return ""
	}, clock, time.Unix(0, 0).Add(time.Duration(parse.CalCeiling*float64(time.Second))), func(acc string) bool {
		return parse.CalTerminal(acc) != ""
	}, nil, sleep)
	if parse.CalTerminal(text) != "done" {
		t.Fatal(text)
	}
	if now.Sub(time.Unix(0, 0)) >= 5*time.Second {
		t.Fatalf("too slow %s", now.Sub(time.Unix(0, 0)))
	}

	now = time.Unix(0, 0)
	emitted = false
	text = Collect(func() string {
		if now.Unix() >= 90 && !emitted {
			emitted = true
			return "cal: hold -\ncal done\ntouch cal ESP_OK\n"
		}
		return ""
	}, clock, time.Unix(0, 0).Add(time.Duration(parse.CalCeiling*float64(time.Second))), func(acc string) bool {
		return parse.CalTerminal(acc) != ""
	}, nil, sleep)
	if parse.CalTerminal(text) != "done" {
		t.Fatal(text)
	}
	elapsed := now.Sub(time.Unix(0, 0))
	if elapsed < 90*time.Second || elapsed >= time.Duration(parse.CalCeiling*float64(time.Second)) {
		t.Fatalf("elapsed %s", elapsed)
	}
}
