package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/image"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/session"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/tui"
	"github.com/ludagoo/SearaBoom/factory_flasher/web"
)

// Version is the flasher program version (not firmware). Set at build time.
var Version = "dev"

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	fs := flag.NewFlagSet("searaboom-factory-flasher", flag.ContinueOnError)
	factoryURL := fs.String("factory-url", envOr("SEARABOOM_FACTORY_URL", image.DefaultFactoryURL), "live factory server (firmware comes from here)")
	imageDir := fs.String("image-dir", os.Getenv("SEARABOOM_FACTORY_DIR"), "local factory slot override (dev only; skips live fetch)")
	noDownload := fs.Bool("no-download", false, "do not contact the live server (requires --image-dir)")
	demo := fs.Bool("demo", false, "TUI walkthrough without USB writes")
	showVersion := fs.Bool("version", false, "print flasher program version and exit")
	linuxSerial := fs.Bool("linux-serial", false, "print Linux udev/group setup and exit")
	installLinuxSerial := fs.Bool("install-linux-serial", false, "install Linux udev rule (needs sudo) and exit")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *showVersion {
		fmt.Printf("searaboom-factory-flasher %s %s/%s\n", Version, runtime.GOOS, runtime.GOARCH)
		return 0
	}
	rules, err := web.UdevRules()
	if err != nil {
		fmt.Fprintf(os.Stderr, "udev rules: %v\n", err)
		return 2
	}
	if *linuxSerial {
		printLinuxSerial(rules)
		return 0
	}
	if *installLinuxSerial {
		return installUdev(rules)
	}

	var sess *session.Session
	if *demo {
		sess = session.Demo()
		fmt.Fprintln(os.Stderr, "DEMO mode — no USB writes, no live firmware download.")
	} else if *imageDir != "" || *noDownload {
		if *imageDir == "" {
			fmt.Fprintln(os.Stderr, "factory image: --no-download requires --image-dir")
			return 2
		}
		src, err := image.LocalOverride(*imageDir)
		if err != nil {
			fmt.Fprintf(os.Stderr, "factory image: %v\n", err)
			return 2
		}
		sess = session.New(session.Options{
			ImageDir: src.Dir,
			ImageVer: src.Version,
			Source:   "local override " + src.Dir,
		})
		fmt.Fprintf(os.Stderr, "local factory image %s at %s (not checking live server)\n", src.Version, src.Dir)
	} else {
		live := image.NewLive(*factoryURL, image.DefaultCacheDir(), image.HTTPGetter("searaboom-factory-flasher/"+Version))
		sess = session.New(session.Options{Live: live})
		if err := sess.RefreshImage(); err != nil {
			fmt.Fprintf(os.Stderr, "live factory image: %v\n", err)
			fmt.Fprintln(os.Stderr, "TUI will start; ARM will retry the live server.")
		} else {
			fmt.Fprintf(os.Stderr, "live factory image %s from %s\n", live.Version, live.BaseURL)
		}
	}

	if err := tui.EnsureTerminal(); err != nil {
		fmt.Fprintf(os.Stderr, "%v\n", err)
		return 2
	}

	stopPoll := session.StartPoll(sess, 400*time.Millisecond)
	defer stopPoll()
	defer sess.Stop()

	if err := tui.Run(sess, Version); err != nil {
		fmt.Fprintf(os.Stderr, "tui: %v\n", err)
		return 1
	}
	return 0
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func printLinuxSerial(rules []byte) {
	fmt.Print(`Linux USB serial (once per factory PC):

sudo tee /etc/udev/rules.d/99-searaboom-esp.rules >/dev/null <<'EOF'
`)
	_, _ = os.Stdout.Write(rules)
	fmt.Print(`EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty || true
sudo usermod -aG uucp "$USER"
# then log out and back in

Or run: searaboom-factory-flasher --install-linux-serial
`)
}

func installUdev(rules []byte) int {
	dst := "/etc/udev/rules.d/99-searaboom-esp.rules"
	cmd := exec.Command("sudo", "tee", dst)
	cmd.Stdin = bytes.NewReader(rules)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "install udev: %v\n", err)
		return 1
	}
	_ = exec.Command("sudo", "udevadm", "control", "--reload-rules").Run()
	_ = exec.Command("sudo", "udevadm", "trigger", "--subsystem-match=tty").Run()
	user := os.Getenv("USER")
	if user != "" {
		_ = exec.Command("sudo", "usermod", "-aG", "uucp", user).Run()
	}
	fmt.Fprintln(os.Stderr, "udev rule installed. Log out/in so group uucp applies.")
	return 0
}
