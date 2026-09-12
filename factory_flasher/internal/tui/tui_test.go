package tui

import (
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/session"
)

func TestRenderHasNoBrowserPath(t *testing.T) {
	sess := session.Demo()
	out := Render(sess.Snapshot(), renderOpts{toolVer: "dev", width: 80})
	if strings.Contains(out, "127.0.0.1") || strings.Contains(strings.ToLower(out), "browser") {
		t.Fatalf("TUI still mentions browser:\n%s", out)
	}
	if strings.Contains(out, "0.5.20") {
		t.Fatal("pinned firmware version in TUI")
	}
	if !strings.Contains(out, "ARM") {
		t.Fatalf("missing ARM:\n%s", out)
	}
}

func TestSpaceThenYArmsAndDemoPass(t *testing.T) {
	sess := session.Demo()
	m := newModel(sess, "dev")
	next, _ := m.Update(tea.KeyMsg{Type: tea.KeySpace})
	m = next.(model)
	if !m.confirmArm {
		t.Fatal("space should ask to confirm ARM")
	}
	if strings.Contains(m.View(), "0.5.20") {
		t.Fatal("confirm view pinned firmware")
	}
	if !strings.Contains(m.View(), "ARM factory flash") {
		t.Fatalf("confirm:\n%s", m.View())
	}
	next, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'y'}})
	m = next.(model)
	if !sess.Snapshot().Armed {
		t.Fatal("Y should ARM")
	}
	sess.Tick()
	deadline := time.Now().Add(3 * time.Second)
	for sess.Snapshot().Phase != "pass" && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	snap := sess.Snapshot()
	if snap.Phase != "pass" || snap.BoxesDone != 1 {
		t.Fatalf("demo walk %+v", snap)
	}
	out := Render(snap, renderOpts{toolVer: "dev", width: 80})
	if !strings.Contains(out, "PASS") {
		t.Fatalf("pass view:\n%s", out)
	}
	if !strings.Contains(out, "0.0.0") {
		t.Fatalf("demo version missing:\n%s", out)
	}
}

func TestQQuits(t *testing.T) {
	sess := session.Demo()
	m := newModel(sess, "dev")
	_, cmd := m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'q'}})
	if cmd == nil {
		t.Fatal("q should quit")
	}
}
