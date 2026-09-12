package session

import (
	"sync"
	"time"
)

// StartPoll ticks the arm → plug-in machine on interval. The TUI only
// reads Snapshot(); flash work stays on this goroutine so the UI stays live.
func StartPoll(s *Session, interval time.Duration) (stop func()) {
	var once sync.Once
	done := make(chan struct{})
	go func() {
		t := time.NewTicker(interval)
		defer t.Stop()
		for {
			select {
			case <-done:
				return
			case <-t.C:
				s.Tick()
			}
		}
	}()
	return func() {
		once.Do(func() { close(done) })
	}
}
