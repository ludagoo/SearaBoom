// Package parse extracts progress, version, board, and touch-cal prompts
// from esptool-style and firmware serial lines.
package parse

import (
	"regexp"
	"strings"
)

const CalCeiling = 110.0

var (
	progressRE   = regexp.MustCompile(`\((\d+)\s*%\)`)
	versionRE    = regexp.MustCompile(`version=([0-9]+\.[0-9]+\.[0-9]+)`)
	boardRE      = regexp.MustCompile(`board=([^\s]+)`)
	calHoldPlus  = regexp.MustCompile(`(?i)^cal:\s*hold\s*\+`)
	calHoldMinus = regexp.MustCompile(`(?i)^cal:\s*hold\s*-`)
	calHandsOff  = regexp.MustCompile(`(?i)^cal:\s*hands off`)
	calDone      = regexp.MustCompile(`(?i)^cal done|touch cal ESP_OK`)
	calFail      = regexp.MustCompile(`(?i)^cal fail|touch cal ESP_FAIL`)
)

func ProgressLine(line string) (int, bool) {
	m := progressRE.FindStringSubmatch(line)
	if m == nil {
		return 0, false
	}
	n := 0
	for _, c := range m[1] {
		n = n*10 + int(c-'0')
	}
	if n < 0 {
		n = 0
	}
	if n > 100 {
		n = 100
	}
	return n, true
}

func VersionLine(line string) string {
	m := versionRE.FindStringSubmatch(line)
	if m == nil {
		return ""
	}
	return m[1]
}

func BoardLine(line string) string {
	m := boardRE.FindStringSubmatch(line)
	if m == nil {
		return ""
	}
	return m[1]
}

func CalPrompt(line string) string {
	if strings.Contains(line, "\n") {
		last := ""
		for _, part := range strings.Split(line, "\n") {
			if p := CalPrompt(part); p != "" {
				last = p
			}
		}
		return last
	}
	text := strings.TrimSpace(line)
	switch {
	case calFail.MatchString(text):
		return "fail"
	case calDone.MatchString(text):
		return "done"
	case calHoldPlus.MatchString(text):
		return "hold+"
	case calHoldMinus.MatchString(text):
		return "hold-"
	case calHandsOff.MatchString(text):
		return "hands-off"
	default:
		return ""
	}
}

func CalTerminal(text string) string {
	last := ""
	for _, part := range strings.Split(text, "\n") {
		p := CalPrompt(part)
		if p == "done" || p == "fail" {
			last = p
		}
	}
	return last
}
