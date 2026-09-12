package main

import (
	"bytes"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/httpserver"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/image"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/ports"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/session"
	"github.com/ludagoo/SearaBoom/factory_flasher/web"
)

// Version is the flasher program version (not firmware). Set at build time.
var Version = "dev"

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	fs := flag.NewFlagSet("searaboom-factory-flasher", flag.ContinueOnError)
	host := fs.String("host", "127.0.0.1", "UI bind address")
	port := fs.Int("port", 8765, "UI port")
	factoryURL := fs.String("factory-url", envOr("SEARABOOM_FACTORY_URL", image.DefaultFactoryURL), "live factory server (firmware comes from here)")
	imageDir := fs.String("image-dir", os.Getenv("SEARABOOM_FACTORY_DIR"), "local factory slot override (dev only; skips live fetch)")
	noDownload := fs.Bool("no-download", false, "do not contact the live server (requires --image-dir)")
	noBrowser := fs.Bool("no-browser", false, "do not open a browser")
	demo := fs.Bool("demo", false, "UI walkthrough without USB writes")
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

	indexHTML, err := web.IndexHTML()
	if err != nil {
		fmt.Fprintf(os.Stderr, "ui: %v\n", err)
		return 2
	}

	var sess *session.Session
	if *demo {
		sess = demoSession()
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
			fmt.Fprintln(os.Stderr, "UI will start; ARM will retry the live server.")
		} else {
			fmt.Fprintf(os.Stderr, "live factory image %s from %s\n", live.Version, live.BaseURL)
		}
	}

	stopPoll := httpserver.StartPoll(sess, 400*time.Millisecond)
	ln, srv, err := httpserver.Serve(sess, indexHTML, *host, *port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen: %v\n", err)
		return 2
	}
	uiURL := "http://" + ln.Addr().String() + "/"
	fmt.Fprintf(os.Stderr, "factory flasher UI %s\n", uiURL)
	if !*noBrowser {
		openBrowser(uiURL)
	}
	go func() {
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "http: %v\n", err)
		}
	}()

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
	<-ch
	fmt.Fprintln(os.Stderr, "stopping")
	stopPoll()
	sess.Stop()
	_ = srv.Close()
	return 0
}

func demoSession() *session.Session {
	portsFn := func() ([]ports.Port, error) {
		return []ports.Port{{
			Device:       "/dev/ttyACM-demo",
			Serial:       "DEMOBOX",
			VID:          0x303A,
			PID:          0x1001,
			Manufacturer: "Espressif",
			Product:      "USB JTAG/serial debug unit",
			HWID:         "USB VID:PID=303A:1001 SER=DEMOBOX",
		}}, nil
	}
	flashFn := func(port, imageDir string, onLine func(string)) error {
		for _, line := range []string{
			"Chip is ESP32-S3",
			"Configuring flash size...",
			"Writing at 0x00000000... (12 %)",
			"Writing at 0x00008000... (28 %)",
			"Writing at 0x0000f000... (41 %)",
			"Writing at 0x00020000... (67 %)",
			"Writing at 0x003a0000... (100 %)",
			"Hash of data verified.",
			"Hard resetting via RTS pin...",
		} {
			onLine(line)
			time.Sleep(40 * time.Millisecond)
		}
		return nil
	}
	serialFn := func(port, command string, wait time.Duration) string {
		switch command {
		case "ver":
			return "version=0.0.0 kconfig=0.0.0 board=s3-zero\n"
		case "board":
			return "board=s3-zero i2s dout=6 bclk=7 ws=8 vol+=3 vol-=2 led=21+48 rise21=0 rise47=0 rise48=0 pulled_low=none\n"
		case "touch cal":
			time.Sleep(40 * time.Millisecond)
			return "touch cal: hold + then -\ncal: hands off\ncal: hold +\ncal: + peak=0.180\ncal: hold -\ncal: - peak=0.175\ncal done vol+=0.144 vol-=0.140\ntouch cal ESP_OK\n"
		default:
			return ""
		}
	}
	return session.New(session.Options{
		ImageDir:  "/tmp/searaboom-factory-demo",
		ImageVer:  "0.0.0",
		Source:    "demo (no live download)",
		ListPorts: portsFn,
		Flash:     flashFn,
		Serial:    serialFn,
		WaitPort:  func(string, time.Duration) bool { return true },
		QAPaths:   []string{},
		Sleep:     func(time.Duration) {},
	})
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

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	_ = cmd.Start()
}
