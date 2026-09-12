package parse

import "testing"

func TestParsers(t *testing.T) {
	if n, ok := ProgressLine("Writing at 0x00020000... (67 %)"); !ok || n != 67 {
		t.Fatalf("progress=%d ok=%v", n, ok)
	}
	if _, ok := ProgressLine("hello"); ok {
		t.Fatal("expected no progress")
	}
	if VersionLine("version=0.5.20 kconfig=0.5.20 board=s3-zero") != "0.5.20" {
		t.Fatal("version")
	}
	if BoardLine("board=s3-supermini i2s dout=6") != "s3-supermini" {
		t.Fatal("board")
	}
	if CalPrompt("cal: hold +") != "hold+" {
		t.Fatal("hold+")
	}
	if CalPrompt("cal: hold -") != "hold-" {
		t.Fatal("hold-")
	}
	if CalPrompt("touch cal ESP_OK") != "done" {
		t.Fatal("done")
	}
	if CalPrompt("cal fail: no +") != "fail" {
		t.Fatal("fail")
	}
	if CalPrompt("touch cal: hold + then -") != "" {
		t.Fatal("intro line should not be a prompt")
	}
	if CalTerminal("touch cal: hold + then -\ncal: hands off\n") != "" {
		t.Fatal("no terminal yet")
	}
	if CalTerminal("cal: hold +\ncal done\ntouch cal ESP_OK") != "done" {
		t.Fatal("terminal done")
	}
	if CalTerminal("cal fail: no +\ntouch cal ESP_FAIL") != "fail" {
		t.Fatal("terminal fail")
	}
}
