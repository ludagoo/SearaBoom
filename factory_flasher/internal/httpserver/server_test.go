package httpserver

import "testing"

func TestRequestIsLocal(t *testing.T) {
	if !RequestIsLocal("127.0.0.1:8765", "") {
		t.Fatal("local host")
	}
	if !RequestIsLocal("127.0.0.1:8765", "http://127.0.0.1:8765") {
		t.Fatal("local origin")
	}
	if RequestIsLocal("127.0.0.1:8765", "http://evil.example") {
		t.Fatal("evil origin")
	}
	if RequestIsLocal("evil.example", "") {
		t.Fatal("evil host")
	}
}
