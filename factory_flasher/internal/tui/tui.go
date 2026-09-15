// Package tui is the factory-flasher operator screen.
// Same keys and layout on Linux, Windows, and macOS. No browser.
package tui

import (
	"fmt"
	"strings"
	"time"
	"unicode/utf8"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/session"
)

const pollEvery = 200 * time.Millisecond

const (
	reset      = "\033[0m"
	bold       = "\033[1m"
	dim        = "\033[2m"
	blackGold  = "\033[30;103m" // ARM
	blackCyan  = "\033[30;106m" // PLUG / HOLD
	blackYell  = "\033[30;103m" // FLASH
	blackGreen = "\033[30;102m" // PASS
	whiteRed   = "\033[97;41m"  // FAIL
	goldText   = "\033[1;93m"
)

// AutoFlashWarn is shown only while ARM'd.
const AutoFlashWarn = "While ARM'd, any ESP32-S3 plugged into this computer is flashed."

type tickMsg struct{}

type model struct {
	sess    *session.Session
	toolVer string
	snap    session.State
	width   int
	height  int
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
	case " ", "a":
		m.sess.Arm(!m.snap.Armed)
	case "d":
		if m.snap.Armed {
			m.sess.Arm(false)
		}
	case "r":
		m.sess.RequestCalRetry()
	case "f", "F":
		if !m.snap.Armed && (m.snap.Phase == "idle" || m.snap.Phase == "watching") {
			go m.sess.FlashOnce()
		}
	}
	m.snap = m.sess.Snapshot()
	return m, nil
}

func (m model) View() string {
	return Render(m.snap, renderOpts{toolVer: m.toolVer, width: m.width})
}

type renderOpts struct {
	toolVer string
	width   int
}

type face struct {
	label string
	style string
	hint  string
}

// Render is the operator screen as plain text (used by the TUI and tests).
func Render(s session.State, opts renderOpts) string {
	w := opts.width
	if w < 40 {
		w = 40
	}
	if w > 72 {
		w = 72
	}

	st := faceFor(s)
	var b strings.Builder
	b.WriteString("\n")
	b.WriteString(banner(st.label, st.style, w))
	b.WriteString("\n")
	if s.Armed {
		fmt.Fprintf(&b, "  %s%s%s\n\n", goldText, AutoFlashWarn, reset)
	}

	if line := statusLine(s); line != "" {
		fmt.Fprintf(&b, "  %s%s%s\n", dim, line, reset)
	}
	if err := errorLine(s); err != "" {
		fmt.Fprintf(&b, "  %s%s%s%s\n", bold, whiteRed, err, reset)
	}
	if st.hint != "" {
		fmt.Fprintf(&b, "  %s\n", st.hint)
	}
	fmt.Fprintf(&b, "  %s\n", keys(s))
	return b.String()
}

func faceFor(s session.State) face {
	switch s.Phase {
	case "flashing", "verify", "detected":
		return face{
			label: "FLASH",
			style: blackYell,
			hint:  fmt.Sprintf("%d%%  %s", s.Progress, progressBar(s.Progress, 24)),
		}
	case "calibrate":
		return face{label: calLabel(s), style: blackCyan, hint: calHint(s)}
	case "pass":
		n := s.BoxesDone
		hint := "unplug"
		if s.Armed {
			hint = "unplug · next box"
		}
		if n > 0 {
			hint = fmt.Sprintf("%d done · %s", n, hint)
		}
		return face{label: "PASS", style: blackGreen, hint: hint}
	case "fail":
		return face{label: "FAIL", style: whiteRed, hint: failHint(s)}
	}
	if !s.Armed {
		hint := ""
		if !s.ImageReady {
			hint = "no image — Space retries"
		}
		return face{label: "ARM", style: blackGold, hint: hint}
	}
	return face{label: "PLUG", style: blackCyan, hint: "plug an ESP32-S3 — it flashes"}
}

func calLabel(s session.State) string {
	switch s.CalPrompt {
	case "hold+":
		return "HOLD +"
	case "hold-":
		return "HOLD −"
	case "hands-off":
		return "HANDS OFF"
	case "fail":
		return "CAL FAIL"
	case "done":
		return "PASS"
	default:
		return "CAL"
	}
}

func calHint(s session.State) string {
	switch s.CalPrompt {
	case "hold+":
		return "hold volume + until it beeps"
	case "hold-":
		return "hold volume − until it beeps"
	case "hands-off":
		return "hands off both pads"
	case "fail":
		return "R  retry"
	default:
		return ""
	}
}

func failHint(s session.State) string {
	if s.CalPrompt == "fail" {
		return "R  retry"
	}
	if s.Armed {
		return "unplug · check cable · Space to disarm"
	}
	return "unplug · check cable · F flash again"
}

func statusLine(s session.State) string {
	ver := s.ImageVersion
	if ver == "" {
		ver = "—"
	}
	parts := []string{ver}
	if s.Port != "" {
		parts = append(parts, s.Port)
	}
	if s.Phase == "flashing" || s.Phase == "verify" {
		parts = append(parts, fmt.Sprintf("%d%%", s.Progress))
	}
	return strings.Join(parts, "  ·  ")
}

func errorLine(s session.State) string {
	if s.Phase != "fail" && s.CalPrompt != "fail" {
		return ""
	}
	if s.LastError != "" {
		return s.LastError
	}
	if s.Message != "" && s.Phase == "fail" {
		return s.Message
	}
	return ""
}

func keys(s session.State) string {
	if s.Armed {
		return dim + "Space  disarm    Q  quit" + reset
	}
	return dim + "Space  ARM batch    F  flash this box    Q  quit" + reset
}

func banner(label, style string, width int) string {
	inner := width - 4
	if inner < 16 {
		inner = 16
	}
	label = strings.TrimSpace(label)
	n := utf8.RuneCountInString(label)
	if n > inner {
		inner = n
	}
	left := (inner - n) / 2
	right := inner - n - left
	blank := style + strings.Repeat(" ", inner) + reset
	mid := style + strings.Repeat(" ", left) + label + strings.Repeat(" ", right) + reset
	indent := "  "
	return indent + blank + "\n" + indent + blank + "\n" + indent + mid + "\n" + indent + blank + "\n" + indent + blank + "\n"
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
	return strings.Repeat("█", fill) + strings.Repeat("░", width-fill)
}
