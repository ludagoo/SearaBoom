// Package session is the arm → detect → flash → confirm state machine.
package session

import (
	"fmt"
	"sync"
	"time"

	"github.com/ludagoo/SearaBoom/factory_flasher/internal/flash"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/image"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/parse"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/ports"
	"github.com/ludagoo/SearaBoom/factory_flasher/internal/serialcmd"
)

type ConfirmStep struct {
	ID     string `json:"id"`
	Title  string `json:"title"`
	Detail string `json:"detail"`
	Status string `json:"status"`
}

type State struct {
	Armed        bool          `json:"armed"`
	Phase        string        `json:"phase"`
	Port         string        `json:"port"`
	Identity     string        `json:"identity"`
	Progress     int           `json:"progress"`
	Message      string        `json:"message"`
	ImageDir     string        `json:"image_dir"`
	ImageVersion string        `json:"image_version"`
	ImageReady   bool          `json:"image_ready"`
	ImageSource  string        `json:"image_source"`
	Log          []string      `json:"log"`
	Confirm      []ConfirmStep `json:"confirm"`
	BoxesDone    int           `json:"boxes_done"`
	LastError    string        `json:"last_error"`
	CalPrompt    string        `json:"cal_prompt"`
}

func defaultConfirm() []ConfirmStep {
	return []ConfirmStep{
		{ID: "ver", Title: "Firmware version", Status: "pending"},
		{ID: "board", Title: "Board detect", Status: "pending"},
		{ID: "cal", Title: "Button calibration (hold + then −)", Status: "pending"},
	}
}

type FlashFunc func(device, imageDir string, onLine func(string)) error
type SerialFunc func(port, command string, wait time.Duration) string
type WaitFunc func(device string, timeout time.Duration) bool
type SleepFunc func(time.Duration)
type NowFunc func() time.Time

type Session struct {
	mu          sync.Mutex
	state       State
	live        *image.Source
	listPorts   ports.Lister
	flash       FlashFunc
	serial      SerialFunc
	waitPort    WaitFunc
	qaPaths     []string
	qaSerials   []string
	sleep       SleepFunc
	now         NowFunc
	baseline    map[string]struct{}
	doneIDs     map[string]struct{}
	busy        bool
	busyID      string
	stop        bool
	oneshot     bool
	wantOnce    bool
	calRetry    chan struct{}
	portByIdent map[string]ports.Port
}

type Options struct {
	Live      *image.Source
	ImageDir  string
	ImageVer  string
	Source    string
	ListPorts ports.Lister
	Flash     FlashFunc
	Serial    SerialFunc
	WaitPort  WaitFunc
	QAPaths   []string
	QASerials []string
	Sleep     SleepFunc
	Now       NowFunc
}

func New(opts Options) *Session {
	src := opts.Source
	if src == "" {
		if opts.Live != nil {
			src = "live " + opts.Live.BaseURL
		} else {
			src = "local"
		}
	}
	ready := opts.ImageDir != ""
	list := opts.ListPorts
	if list == nil {
		list = ports.List
	}
	sleep := opts.Sleep
	if sleep == nil {
		sleep = time.Sleep
	}
	now := opts.Now
	if now == nil {
		now = time.Now
	}
	wait := opts.WaitPort
	if wait == nil {
		wait = flash.Wait
	}
	return &Session{
		state: State{
			Phase:        "idle",
			Message:      "Disarmed. Arm when you are ready to flash.",
			ImageDir:     opts.ImageDir,
			ImageVersion: opts.ImageVer,
			ImageReady:   ready,
			ImageSource:  src,
			Confirm:      defaultConfirm(),
		},
		live:        opts.Live,
		listPorts:   list,
		flash:       opts.Flash,
		serial:      opts.Serial,
		waitPort:    wait,
		qaPaths:     opts.QAPaths,
		qaSerials:   opts.QASerials,
		sleep:       sleep,
		now:         now,
		baseline:    map[string]struct{}{},
		doneIDs:     map[string]struct{}{},
		calRetry:    make(chan struct{}, 1),
		portByIdent: map[string]ports.Port{},
	}
}

func (s *Session) Snapshot() State {
	s.mu.Lock()
	defer s.mu.Unlock()
	cp := s.state
	cp.Log = append([]string(nil), s.state.Log...)
	if len(cp.Log) > 80 {
		cp.Log = cp.Log[len(cp.Log)-80:]
	}
	cp.Confirm = append([]ConfirmStep(nil), s.state.Confirm...)
	return cp
}

func (s *Session) logf(line string) {
	line = trimNL(line)
	if line == "" {
		return
	}
	s.state.Log = append(s.state.Log, line)
	if len(s.state.Log) > 400 {
		s.state.Log = s.state.Log[len(s.state.Log)-300:]
	}
}

func (s *Session) eligible() []ports.Port {
	list, err := s.listPorts()
	if err != nil {
		s.logf("port list: " + err.Error())
		return nil
	}
	var qaPaths []string
	var qaSerials []string
	if s.qaPaths != nil {
		qaPaths = s.qaPaths
		qaSerials = s.qaSerials
	}
	out := ports.Eligible(list, qaPaths, qaSerials)
	for _, p := range out {
		s.portByIdent[p.Identity()] = p
	}
	return out
}

func (s *Session) RefreshImage() error {
	if s.live == nil {
		s.mu.Lock()
		s.state.ImageReady = s.state.ImageDir != ""
		s.mu.Unlock()
		return nil
	}
	if err := s.live.Refresh(); err != nil {
		s.mu.Lock()
		s.state.ImageReady = false
		s.state.LastError = err.Error()
		s.state.Message = "Live factory image: " + err.Error()
		s.mu.Unlock()
		return err
	}
	s.mu.Lock()
	s.state.ImageDir = s.live.Dir
	s.state.ImageVersion = s.live.Version
	s.state.ImageReady = true
	s.state.ImageSource = "live " + s.live.BaseURL
	s.logf("live factory image " + s.live.Version + " from " + s.live.BaseURL)
	s.mu.Unlock()
	return nil
}

func (s *Session) Arm(armed bool) State {
	if armed {
		if err := s.RefreshImage(); err != nil {
			s.mu.Lock()
			defer s.mu.Unlock()
			s.state.Armed = false
			s.state.Message = "No factory image. Cannot arm. " + err.Error()
			return copyState(s.state)
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if armed && !s.state.ImageReady {
		s.state.Message = "No factory image. Cannot arm."
		return copyState(s.state)
	}
	s.state.Armed = armed
	if armed {
		plist := s.eligible()
		s.baseline = map[string]struct{}{}
		for _, p := range plist {
			s.baseline[p.Identity()] = struct{}{}
		}
		var already []ports.Port
		for _, p := range plist {
			if _, done := s.doneIDs[p.Identity()]; !done {
				already = append(already, p)
			}
		}
		s.state.Phase = "watching"
		s.state.LastError = ""
		switch len(already) {
		case 1:
			delete(s.baseline, already[0].Identity())
			s.state.Message = "Armed. Flashing the box on " + already[0].Device + "."
		case 0:
			s.state.Message = "Armed. Plug in a SearaBoom to flash it."
		default:
			s.state.Message = "Armed. Unplug extra boxes, then plug one at a time."
		}
	} else {
		if s.state.Phase != "flashing" && s.state.Phase != "calibrate" {
			s.state.Phase = "idle"
			s.state.Port = ""
			s.state.Identity = ""
			s.state.Progress = 0
			s.state.CalPrompt = ""
			s.state.Confirm = defaultConfirm()
		}
		s.state.Message = "Disarmed. Arm when you are ready to flash."
	}
	return copyState(s.state)
}

// FlashOnce flashes the ESP32-S3 that is already plugged in, then stays
// disarmed so the next plug-in does not auto-flash.
func (s *Session) FlashOnce() State {
	s.mu.Lock()
	if !s.canStartOnceLocked() {
		st := copyState(s.state)
		s.mu.Unlock()
		return st
	}
	s.mu.Unlock()
	if err := s.RefreshImage(); err != nil {
		s.mu.Lock()
		defer s.mu.Unlock()
		s.state.LastError = err.Error()
		s.state.Message = "No factory image. " + err.Error()
		return copyState(s.state)
	}
	s.mu.Lock()
	if !s.canStartOnceLocked() {
		st := copyState(s.state)
		s.mu.Unlock()
		return st
	}
	s.wantOnce = true
	s.mu.Unlock()
	s.Tick()
	return s.Snapshot()
}

func (s *Session) canStartOnceLocked() bool {
	if s.busy || s.oneshot || s.wantOnce || s.state.Armed {
		return false
	}
	return s.state.Phase == "idle" || s.state.Phase == "watching"
}

func (s *Session) workOK() bool {
	return s.state.Armed || s.oneshot
}

func (s *Session) RequestCalRetry() State {
	select {
	case s.calRetry <- struct{}{}:
	default:
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.logf("operator requested calibration retry")
	return copyState(s.state)
}

func (s *Session) Stop() {
	s.stop = true
}

func (s *Session) Tick() {
	s.mu.Lock()
	if s.busy {
		s.wantOnce = false
		s.mu.Unlock()
		return
	}
	if !s.state.Armed && (s.state.Phase == "pass" || s.state.Phase == "fail") && s.state.Identity != "" {
		still := false
		for _, p := range s.eligible() {
			if p.Identity() == s.state.Identity {
				still = true
				break
			}
		}
		if !still {
			s.state.Phase = "idle"
			s.state.Port = ""
			s.state.Identity = ""
			s.state.Progress = 0
			s.state.CalPrompt = ""
			s.state.Confirm = defaultConfirm()
			s.state.Message = "Disarmed. Arm when you are ready to flash."
		}
	}
	if s.wantOnce && !s.state.Armed {
		s.wantOnce = false
		s.startOnceLocked()
		return
	}
	if !s.state.Armed {
		s.mu.Unlock()
		return
	}
	if s.state.Phase == "flashing" || s.state.Phase == "verify" || s.state.Phase == "calibrate" {
		s.mu.Unlock()
		return
	}
	plist := s.eligible()
	var fresh []ports.Port
	for _, p := range ports.NewAmong(plist, s.baseline) {
		if _, done := s.doneIDs[p.Identity()]; done {
			continue
		}
		if p.Identity() == s.busyID {
			continue
		}
		fresh = append(fresh, p)
	}
	if len(fresh) == 0 {
		if s.state.Phase == "pass" || s.state.Phase == "fail" {
			still := map[string]struct{}{}
			for _, p := range plist {
				still[p.Identity()] = struct{}{}
			}
			if s.state.Identity != "" {
				if _, ok := still[s.state.Identity]; !ok {
					delete(s.baseline, s.state.Identity)
					s.state.Phase = "watching"
					s.state.Port = ""
					s.state.Identity = ""
					s.state.Progress = 0
					s.state.CalPrompt = ""
					s.state.Confirm = defaultConfirm()
					s.state.Message = "Armed. Plug in the next box."
				}
			}
		}
		s.mu.Unlock()
		return
	}
	target := fresh[0]
	s.busy = true
	s.busyID = target.Identity()
	s.state.Phase = "detected"
	s.state.Port = target.Device
	s.state.Identity = target.Identity()
	s.state.Progress = 0
	s.state.LastError = ""
	s.state.Confirm = defaultConfirm()
	s.state.CalPrompt = ""
	s.state.Message = "Detected " + target.Device + ". Starting flash."
	s.logf("detected " + target.Device + " id=" + target.Identity())
	s.mu.Unlock()
	s.runBox(target)
}

// startOnceLocked runs with s.mu held and unlocks before return.
func (s *Session) startOnceLocked() {
	if !s.state.ImageReady {
		s.state.Message = "No factory image."
		s.mu.Unlock()
		return
	}
	already := s.eligible()
	switch len(already) {
	case 0:
		s.state.Message = "No ESP32-S3 plugged in."
		s.state.LastError = ""
		s.mu.Unlock()
		return
	case 1:
		target := already[0]
		s.oneshot = true
		s.busy = true
		s.busyID = target.Identity()
		s.state.Phase = "detected"
		s.state.Port = target.Device
		s.state.Identity = target.Identity()
		s.state.Progress = 0
		s.state.LastError = ""
		s.state.Confirm = defaultConfirm()
		s.state.CalPrompt = ""
		s.state.Message = "Flashing " + target.Device + " once."
		s.logf("one-shot " + target.Device + " id=" + target.Identity())
		s.mu.Unlock()
		s.runBox(target)
		s.mu.Lock()
		s.oneshot = false
		s.mu.Unlock()
		return
	default:
		s.state.Message = "Unplug extra boxes, then flash one."
		s.mu.Unlock()
	}
}

func (s *Session) runBox(target ports.Port) {
	defer func() {
		s.mu.Lock()
		s.busy = false
		s.busyID = ""
		s.baseline[target.Identity()] = struct{}{}
		if s.state.Phase == "pass" && target.HasStableIdentity() {
			s.doneIDs[target.Identity()] = struct{}{}
		}
		s.mu.Unlock()
	}()
	if err := s.flashAndConfirm(target); err != nil {
		s.mu.Lock()
		s.logf("error: " + err.Error())
		s.state.Phase = "fail"
		s.state.LastError = err.Error()
		s.state.Message = "Fail: " + err.Error()
		s.state.Progress = 0
		s.mu.Unlock()
	}
}

func (s *Session) onFlashLine(line string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.logf(line)
	if pct, ok := parse.ProgressLine(line); ok {
		s.state.Progress = pct
		s.state.Message = fmt.Sprintf("Flashing… %d%%", pct)
	}
}

func (s *Session) flashAndConfirm(target ports.Port) error {
	s.mu.Lock()
	s.state.Phase = "flashing"
	s.state.Progress = 0
	s.state.Message = "Flashing…"
	imageDir := s.state.ImageDir
	s.mu.Unlock()

	var err error
	if s.flash != nil {
		err = s.flash(target.Device, imageDir, s.onFlashLine)
	} else {
		err = flash.Port(target.Device, imageDir, target, s.onFlashLine)
	}
	if err != nil {
		s.mu.Lock()
		s.state.Phase = "fail"
		s.state.LastError = err.Error()
		s.state.Message = "Flash failed. Unplug, check the USB cable, arm, try again."
		s.mu.Unlock()
		return nil
	}
	s.mu.Lock()
	s.state.Phase = "verify"
	s.state.Progress = 100
	s.state.Message = "Flash wrote. Checking boot…"
	s.logf("flash ok — waiting for serial")
	s.mu.Unlock()
	if !s.waitPort(target.Device, 12*time.Second) {
		s.mu.Lock()
		s.state.Phase = "fail"
		s.state.LastError = "serial did not return after reset"
		s.state.Message = "Flash wrote but the USB serial port did not come back."
		s.mu.Unlock()
		return nil
	}
	s.sleep(2 * time.Second)
	s.verify(target.Device)
	if s.phase() == "fail" {
		return nil
	}
	s.calibrate(target.Device)
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.state.Phase == "fail" {
		return nil
	}
	ok := true
	for _, step := range s.state.Confirm {
		if step.Status != "pass" {
			ok = false
			break
		}
	}
	if ok {
		s.state.BoxesDone++
		s.state.Phase = "pass"
		s.state.Message = "PASS. Unplug this box, then plug the next one."
	} else {
		s.state.Phase = "fail"
		s.state.Message = "Confirmation failed. See steps below."
	}
	return nil
}

func (s *Session) phase() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.state.Phase
}

func (s *Session) runSerial(port, command string, wait time.Duration, stop serialcmd.StopFunc) string {
	onLine := func(line string) {
		s.mu.Lock()
		s.logf(line)
		if p := parse.CalPrompt(line); p != "" {
			s.state.CalPrompt = p
		}
		s.mu.Unlock()
	}
	if s.serial != nil {
		text := s.serial(port, command, wait)
		for _, line := range splitLines(text) {
			onLine(line)
		}
		return text
	}
	text, err := serialcmd.Run(port, command, wait, onLine, stop, nil)
	if err != nil {
		s.mu.Lock()
		s.logf("serial: " + err.Error())
		s.mu.Unlock()
	}
	return text
}

func (s *Session) mark(id, status, detail string) {
	for i := range s.state.Confirm {
		if s.state.Confirm[i].ID == id {
			s.state.Confirm[i].Status = status
			s.state.Confirm[i].Detail = detail
			return
		}
	}
}

func (s *Session) verify(device string) {
	s.mu.Lock()
	s.mark("ver", "active", "sending ver")
	expect := s.state.ImageVersion
	s.mu.Unlock()
	text := s.runSerial(device, "ver", 2*time.Second, nil)
	version := ""
	board := ""
	for _, line := range splitLines(text) {
		if version == "" {
			version = parse.VersionLine(line)
		}
		if board == "" {
			board = parse.BoardLine(line)
		}
	}
	if version == "" {
		s.sleep(800 * time.Millisecond)
		text = s.runSerial(device, "ver", 2*time.Second, nil)
		for _, line := range splitLines(text) {
			if version == "" {
				version = parse.VersionLine(line)
			}
			if board == "" {
				board = parse.BoardLine(line)
			}
		}
	}
	s.mu.Lock()
	if version == "" {
		s.mark("ver", "fail", "no version= line")
		s.state.Phase = "fail"
		s.state.LastError = "no ver"
		s.state.Message = "Box did not answer `ver`."
		s.mu.Unlock()
		return
	}
	match := expect == "" || version == expect
	detail := version
	if !match {
		detail = version + " (wanted " + expect + ")"
	}
	if match {
		s.mark("ver", "pass", detail)
	} else {
		s.mark("ver", "fail", detail)
		s.state.Phase = "fail"
		s.state.LastError = "version mismatch"
		s.state.Message = "Version mismatch after flash."
		s.mu.Unlock()
		return
	}
	s.mu.Unlock()

	boardText := s.runSerial(device, "board", 1500*time.Millisecond, nil)
	for _, line := range splitLines(boardText) {
		if board == "" {
			board = parse.BoardLine(line)
		}
	}
	s.mu.Lock()
	s.mark("board", "active", "sending board")
	if board != "" {
		s.mark("board", "pass", board)
		s.state.Message = "Booted " + board + " " + version + ". Starting pad calibration."
	} else {
		s.mark("board", "fail", "no board= line")
		s.state.Phase = "fail"
		s.state.LastError = "no board"
		s.state.Message = "Box did not answer `board`."
	}
	s.mu.Unlock()
}

func (s *Session) calibrate(device string) {
	for !s.stop {
		s.mu.Lock()
		if !s.workOK() {
			s.state.Phase = "fail"
			s.state.Message = "Disarmed during calibration."
			s.mu.Unlock()
			return
		}
		s.state.Phase = "calibrate"
		s.state.CalPrompt = "hands-off"
		s.state.Message = "Calibration: hands off both pads, then hold + , then −."
		s.mark("cal", "active", "hands off, then hold + then −")
		// drop a stale retry
		select {
		case <-s.calRetry:
		default:
		}
		s.mu.Unlock()

		text := s.runSerial(device, "touch cal", time.Duration(parse.CalCeiling*float64(time.Second)), func(acc string) bool {
			return parse.CalTerminal(acc) != ""
		})
		terminal := parse.CalTerminal(text)
		s.mu.Lock()
		if terminal == "done" {
			s.mark("cal", "pass", "touch cal ESP_OK")
			s.state.CalPrompt = "done"
			s.mu.Unlock()
			return
		}
		if terminal == "fail" {
			s.mark("cal", "fail", "touch cal failed — retry or check pads")
			s.state.CalPrompt = "fail"
			s.state.Message = "Calibration failed. Fix the pads and retry, or disarm."
		} else {
			s.mark("cal", "fail", "no touch cal result before timeout")
			s.state.CalPrompt = "fail"
			s.state.Phase = "fail"
			s.state.LastError = "cal timeout"
			s.state.Message = "Calibration timed out. Unplug and try again — do not retry while the box is still beeping."
			s.mu.Unlock()
			return
		}
		s.mu.Unlock()

		deadline := s.now().Add(120 * time.Second)
		for !s.stop && s.now().Before(deadline) {
			select {
			case <-s.calRetry:
				goto retry
			default:
			}
			s.mu.Lock()
			ok := s.workOK()
			s.mu.Unlock()
			if !ok {
				s.mu.Lock()
				s.state.Phase = "fail"
				s.state.Message = "Disarmed during calibration."
				s.mu.Unlock()
				return
			}
			s.sleep(200 * time.Millisecond)
		}
		s.mu.Lock()
		s.state.Phase = "fail"
		s.state.LastError = "cal timeout"
		s.state.Message = "Calibration failed."
		s.mu.Unlock()
		return
	retry:
		continue
	}
}

func copyState(st State) State {
	st.Log = append([]string(nil), st.Log...)
	if len(st.Log) > 80 {
		st.Log = st.Log[len(st.Log)-80:]
	}
	st.Confirm = append([]ConfirmStep(nil), st.Confirm...)
	return st
}

func trimNL(s string) string {
	for len(s) > 0 && (s[len(s)-1] == '\n' || s[len(s)-1] == '\r') {
		s = s[:len(s)-1]
	}
	return s
}

func splitLines(text string) []string {
	if text == "" {
		return nil
	}
	var out []string
	start := 0
	for i := 0; i < len(text); i++ {
		if text[i] == '\n' {
			line := trimNL(text[start:i])
			if line != "" {
				out = append(out, line)
			}
			start = i + 1
		}
	}
	if start < len(text) {
		if line := trimNL(text[start:]); line != "" {
			out = append(out, line)
		}
	}
	return out
}
