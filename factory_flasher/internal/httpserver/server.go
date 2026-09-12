package httpserver

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/session"
)

var localHosts = map[string]struct{}{
	"127.0.0.1": {},
	"localhost": {},
	"::1":       {},
}

func RequestIsLocal(hostHdr, origin string) bool {
	host := strings.TrimSpace(hostHdr)
	hostname := host
	if strings.HasPrefix(host, "[") {
		if i := strings.Index(host, "]"); i > 0 {
			hostname = host[1:i]
		}
	} else if strings.Count(host, ":") == 1 {
		hostname, _, _ = strings.Cut(host, ":")
	}
	if _, ok := localHosts[strings.ToLower(hostname)]; !ok {
		return false
	}
	origin = strings.TrimSpace(origin)
	if origin == "" {
		return true
	}
	u, err := url.Parse(origin)
	if err != nil {
		return false
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return false
	}
	_, ok := localHosts[strings.ToLower(u.Hostname())]
	return ok
}

func Handler(sess *session.Session, indexHTML []byte) http.Handler {
	mux := http.NewServeMux()
	writeJSON := func(w http.ResponseWriter, code int, payload any) {
		body, _ := json.Marshal(payload)
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(code)
		_, _ = w.Write(body)
	}
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" && r.URL.Path != "/index.html" {
			writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(indexHTML)
	})
	mux.HandleFunc("/api/state", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, sess.Snapshot())
	})
	mux.HandleFunc("/api/arm", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "POST only"})
			return
		}
		if !RequestIsLocal(r.Host, r.Header.Get("Origin")) {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "local origin only"})
			return
		}
		var body struct {
			Armed bool `json:"armed"`
		}
		dec := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
		_ = dec.Decode(&body)
		writeJSON(w, http.StatusOK, sess.Arm(body.Armed))
	})
	mux.HandleFunc("/api/cal/retry", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "POST only"})
			return
		}
		if !RequestIsLocal(r.Host, r.Header.Get("Origin")) {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "local origin only"})
			return
		}
		writeJSON(w, http.StatusOK, sess.RequestCalRetry())
	})
	return mux
}

func Serve(sess *session.Session, indexHTML []byte, host string, port int) (net.Listener, *http.Server, error) {
	ln, err := net.Listen("tcp", net.JoinHostPort(host, itoa(port)))
	if err != nil {
		return nil, nil, err
	}
	srv := &http.Server{
		Handler:           Handler(sess, indexHTML),
		ReadHeaderTimeout: 5 * time.Second,
	}
	return ln, srv, nil
}

func StartPoll(sess *session.Session, interval time.Duration) (stop func()) {
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
				sess.Tick()
			}
		}
	}()
	return func() {
		once.Do(func() { close(done) })
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var b [16]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
