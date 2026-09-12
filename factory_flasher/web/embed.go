package web

import "embed"

//go:embed index.html 99-searaboom-esp.rules
var FS embed.FS

func IndexHTML() ([]byte, error) {
	return FS.ReadFile("index.html")
}

func UdevRules() ([]byte, error) {
	return FS.ReadFile("99-searaboom-esp.rules")
}
