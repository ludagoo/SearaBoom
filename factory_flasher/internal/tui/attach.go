package tui

import (
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"

	"golang.org/x/term"
)

const attachedEnv = "SEARABOOM_FLASHER_ATTACHED"

// EnsureTerminal makes sure the TUI has a console. Double-click on Windows
// already opens one. On Linux/macOS without a TTY, we try to re-open in a
// terminal emulator so the operator does not start a second program by hand.
func EnsureTerminal() error {
	if isTTY() {
		return nil
	}
	if os.Getenv(attachedEnv) == "1" {
		return missingTTY()
	}
	if os.Getenv("CI") != "" || os.Getenv("GITHUB_ACTIONS") != "" {
		return missingTTY()
	}
	if runtime.GOOS == "windows" {
		// Console-subsystem binaries get a window on double-click.
		return missingTTY()
	}
	if runtime.GOOS == "linux" && os.Getenv("DISPLAY") == "" && os.Getenv("WAYLAND_DISPLAY") == "" {
		return missingTTY()
	}
	if attachInTerminal() {
		os.Exit(0)
	}
	return missingTTY()
}

func isTTY() bool {
	return term.IsTerminal(int(os.Stdin.Fd())) && term.IsTerminal(int(os.Stdout.Fd()))
}

func missingTTY() error {
	exe, err := os.Executable()
	if err != nil || exe == "" {
		exe = "searaboom-factory-flasher"
	}
	return fmt.Errorf("this program is a terminal UI. Paste the curl one-liner from https://searaboom.goossen.dev/ in a terminal, or run:\n  %s", exe)
}

func attachInTerminal() bool {
	exe, err := os.Executable()
	if err != nil || exe == "" {
		return false
	}
	args := os.Args[1:]
	env := append(os.Environ(), attachedEnv+"=1")

	if runtime.GOOS == "darwin" {
		script := "export " + attachedEnv + "=1; exec " + shellQuote(exe)
		for _, a := range args {
			script += " " + shellQuote(a)
		}
		cmd := exec.Command("osascript", "-e", `tell application "Terminal" to do script `+shellQuote(script))
		cmd.Env = env
		return cmd.Start() == nil
	}

	type cand struct {
		bin  string
		argv []string
	}
	cands := []cand{
		{"xdg-terminal-exec", append([]string{exe}, args...)},
		{"x-terminal-emulator", append([]string{"-e", exe}, args...)},
		{"gnome-terminal", append([]string{"--", exe}, args...)},
		{"konsole", append([]string{"-e", exe}, args...)},
		{"xfce4-terminal", append([]string{"-e", exe}, args...)},
		{"kitty", append([]string{exe}, args...)},
		{"alacritty", append([]string{"-e", exe}, args...)},
		{"xterm", append([]string{"-e", exe}, args...)},
	}
	for _, c := range cands {
		path, err := exec.LookPath(c.bin)
		if err != nil {
			continue
		}
		cmd := exec.Command(path, c.argv...)
		cmd.Env = env
		if err := cmd.Start(); err == nil {
			return true
		}
	}
	return false
}

func shellQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'"'"'`) + "'"
}
