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
	if strings.Contains(out, AutoFlashWarn) {
		t.Fatalf("warning should only show while ARM'd:\n%s", out)
	}
	if !strings.Contains(out, "F  flash") {
		t.Fatalf("missing one-shot flash:\n%s", out)
	}
	if strings.Contains(out, "Writing at") || strings.Contains(out, "\nLog\n") {
		t.Fatalf("log dump is the main UI:\n%s", out)
	}
}

func TestBigStates(t *testing.T) {
	cases := []struct {
		phase string
		armed bool
		want  string
	}{
		{phase: "idle", armed: false, want: "ARM"},
		{phase: "watching", armed: true, want: "PLUG"},
		{phase: "flashing", armed: true, want: "FLASH"},
		{phase: "pass", armed: true, want: "PASS"},
		{phase: "fail", armed: true, want: "FAIL"},
	}
	for _, tc := range cases {
		st := session.State{Phase: tc.phase, Armed: tc.armed, ImageReady: true, ImageVersion: "0.0.0"}
		if tc.phase == "fail" {
			st.LastError = "cable loose"
		}
		out := Render(st, renderOpts{width: 60})
		if !strings.Contains(out, tc.want) {
			t.Fatalf("%s: want %s in\n%s", tc.phase, tc.want, out)
		}
		if tc.armed && !strings.Contains(out, AutoFlashWarn) {
			t.Fatalf("%s: missing auto-flash warning while armed:\n%s", tc.phase, out)
		}
		if !tc.armed && strings.Contains(out, AutoFlashWarn) {
			t.Fatalf("%s: warning while disarmed:\n%s", tc.phase, out)
		}
		if tc.phase == "fail" && !strings.Contains(out, "cable loose") {
			t.Fatalf("fail hid error:\n%s", out)
		}
		if strings.Contains(out, "0.5.20") {
			t.Fatal("pinned")
		}
	}
}

func TestSpaceArmsAndDemoPass(t *testing.T) {
	sess := session.Demo()
	m := newModel(sess, "dev")
	next, _ := m.Update(tea.KeyMsg{Type: tea.KeySpace})
	m = next.(model)
	if !sess.Snapshot().Armed {
		t.Fatal("Space should ARM")
	}
	out := Render(sess.Snapshot(), renderOpts{width: 60})
	if !strings.Contains(out, "PLUG") {
		t.Fatalf("armed should be PLUG:\n%s", out)
	}
	if !strings.Contains(out, AutoFlashWarn) {
		t.Fatalf("armed should warn:\n%s", out)
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
	out = Render(snap, renderOpts{toolVer: "dev", width: 80})
	if !strings.Contains(out, "PASS") {
		t.Fatalf("pass view:\n%s", out)
	}
	if !strings.Contains(out, "0.0.0") {
		t.Fatalf("demo version missing:\n%s", out)
	}
	if strings.Contains(out, "Writing at") {
		t.Fatalf("pass view dumped log:\n%s", out)
	}
}

func TestCapitalFFlashesOnce(t *testing.T) {
	sess := session.Demo()
	m := newModel(sess, "dev")
	next, _ := m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'F'}})
	m = next.(model)
	deadline := time.Now().Add(3 * time.Second)
	for sess.Snapshot().Phase != "pass" && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if sess.Snapshot().Phase != "pass" || sess.Snapshot().BoxesDone != 1 {
		t.Fatalf("capital F did not one-shot: %+v", sess.Snapshot())
	}
	if sess.Snapshot().Armed {
		t.Fatal("F must not ARM")
	}
	next, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{'F'}})
	_ = next
	time.Sleep(200 * time.Millisecond)
	if sess.Snapshot().BoxesDone != 1 {
		t.Fatalf("second F after PASS flashed again: %d", sess.Snapshot().BoxesDone)
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
