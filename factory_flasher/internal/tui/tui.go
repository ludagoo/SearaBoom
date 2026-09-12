// Package tui is the factory-flasher operator screen.
// Same keys and layout on Linux, Windows, and macOS. No browser.
package tui

import (
	"fmt"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/session"
)

const pollEvery = 200 * time.Millisecond

var calCopy = map[string]string{
	"hands-off": "Hands off both pads.",
	"hold+":     "Hold volume + until it beeps.",
	"hold-":     "Hold volume − until it beeps.",
	"done":      "Calibration saved.",
	"fail":      "Calibration failed. Retry after checking the pads.",
}

type tickMsg struct{}

type model struct {
	sess       *session.Session
	toolVer    string
	snap       session.State
	confirmArm bool
	width      int
	height     int
}

func newModel(sess *session.Session, toolVer string) model {
	return model{
		sess:    sess,
		toolVer: toolVer,
		snap:    sess.Snapshot(),
		width:   80,
		height:  24,
	}
}

// Run opens the terminal UI and blocks until the operator quits.
func Run(sess *session.Session, toolVer string) error {
	p := tea.NewProgram(newModel(sess, toolVer), tea.WithAltScreen())
	_, err := p.Run()
	return err
}

func (m model) Init() tea.Cmd {
	return tea.Tick(pollEvery, func(time.Time) tea.Msg { return tickMsg{} })
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		return m, nil
	case tickMsg:
		m.snap = m.sess.Snapshot()
		return m, tea.Tick(pollEvery, func(time.Time) tea.Msg { return tickMsg{} })
	case tea.KeyMsg:
		return m.handleKey(msg)
	}
	return m, nil
}

func (m model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q":
		return m, tea.Quit
	case " ":
		if m.snap.Armed {
			m.sess.Arm(false)
			m.confirmArm = false
		} else {
			m.confirmArm = true
		}
	case "y":
		if m.confirmArm && !m.snap.Armed {
			m.sess.Arm(true)
			m.confirmArm = false
		}
	case "n", "esc":
		m.confirmArm = false
	case "d":
		if m.snap.Armed {
			m.sess.Arm(false)
			m.confirmArm = false
		}
	case "r":
		m.sess.RequestCalRetry()
	}
	m.snap = m.sess.Snapshot()
	return m, nil
}

func (m model) View() string {
	return Render(m.snap, renderOpts{
		toolVer:    m.toolVer,
		confirmArm: m.confirmArm,
		width:      m.width,
	})
}

type renderOpts struct {
	toolVer    string
	confirmArm bool
	width      int
}

// Render is the operator screen as plain text (used by the TUI and tests).
func Render(s session.State, opts renderOpts) string {
	w := opts.width
	if w < 48 {
		w = 48
	}
	if w > 100 {
		w = 100
	}

	var b strings.Builder
	ver := opts.toolVer
	if ver == "" {
		ver = "dev"
	}
	fmt.Fprintf(&b, "SearaBoom factory flasher  %s\n", ver)
	fmt.Fprintf(&b, "%s\n\n", imageLine(s))

	if opts.confirmArm && !s.Armed {
		b.WriteString("ARM factory flash?\n")
		b.WriteString("Re-checks the live USB image, then flashes the box that is\n")
		b.WriteString("already plugged in (or the next one). QA boxes are skipped.\n\n")
		b.WriteString("[ Y ] yes    [ N ] no\n")
		return b.String()
	}

	if s.Armed {
		b.WriteString("ARMED  — next plug-in flashes.  [ Space / D ] disarm\n")
	} else {
		b.WriteString("[ Space ] ARM — flash the next box I plug in\n")
	}
	b.WriteString("[ R ]     retry calibration    [ Q ] quit\n\n")

	phase := s.Phase
	if s.Port != "" {
		phase = phase + "  ·  " + s.Port
	}
	fmt.Fprintf(&b, "Phase  %s\n", phase)
	if s.Message != "" {
		fmt.Fprintf(&b, "%s\n", s.Message)
	}
	b.WriteByte('\n')

	fmt.Fprintf(&b, "[%s]  %d%%\n\n", progressBar(s.Progress, 28), s.Progress)

	switch s.Phase {
	case "pass":
		fmt.Fprintf(&b, "PASS  (%d this session)\n\n", s.BoxesDone)
	case "fail":
		b.WriteString("FAIL\n")
		if s.LastError != "" {
			fmt.Fprintf(&b, "%s\n", s.LastError)
		}
		b.WriteByte('\n')
	}

	if prompt := calLine(s); prompt != "" {
		fmt.Fprintf(&b, "Cal  %s\n\n", prompt)
	}

	for _, step := range s.Confirm {
		mark := "  "
		switch step.Status {
		case "pass":
			mark = "OK"
		case "fail":
			mark = "!!"
		case "active":
			mark = ">>"
		}
		detail := step.Detail
		if detail != "" {
			detail = "  —  " + detail
		}
		fmt.Fprintf(&b, "  %s  %s%s\n", mark, step.Title, detail)
	}

	b.WriteString("\nLog\n")
	log := s.Log
	if len(log) > 12 {
		log = log[len(log)-12:]
	}
	if len(log) == 0 {
		b.WriteString("  (waiting)\n")
	} else {
		for _, line := range log {
			fmt.Fprintf(&b, "  %s\n", line)
		}
	}
	_ = w
	return b.String()
}

func imageLine(s session.State) string {
	if !s.ImageReady {
		return "No factory image from the live server. ARM will retry."
	}
	ver := s.ImageVersion
	if ver == "" {
		ver = "unknown"
	}
	src := s.ImageSource
	if src == "" {
		src = s.ImageDir
	}
	return fmt.Sprintf("Live USB image %s  ·  ESP32-S3  ·  4MB  ·  %s", ver, src)
}

func calLine(s session.State) string {
	if s.Phase != "calibrate" && s.CalPrompt == "" {
		return ""
	}
	if text, ok := calCopy[s.CalPrompt]; ok {
		return text
	}
	return "Calibration in progress."
}

func progressBar(pct, width int) string {
	if pct < 0 {
		pct = 0
	}
	if pct > 100 {
		pct = 100
	}
	if width < 8 {
		width = 8
	}
	fill := width * pct / 100
	return strings.Repeat("#", fill) + strings.Repeat("-", width-fill)
}
