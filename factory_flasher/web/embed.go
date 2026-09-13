package web

import "embed"

//go:embed 99-searaboom-esp.rules
var FS embed.FS

func UdevRules() ([]byte, error) {
	return FS.ReadFile("99-searaboom-esp.rules")
}
