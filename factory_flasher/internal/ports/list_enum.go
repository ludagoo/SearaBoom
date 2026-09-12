//go:build linux || windows

package ports

import "go.bug.st/serial/enumerator"

func List() ([]Port, error) {
	infos, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, err
	}
	var out []Port
	for _, info := range infos {
		if info == nil {
			continue
		}
		p := Port{
			Device:       info.Name,
			Serial:       info.SerialNumber,
			VID:          parseHexID(info.VID),
			PID:          parseHexID(info.PID),
			Manufacturer: info.Manufacturer,
			Product:      info.Product,
			HWID:         "USB VID:PID=" + info.VID + ":" + info.PID,
		}
		if info.SerialNumber != "" {
			p.HWID += " SER=" + info.SerialNumber
		}
		if IsSearaboom(p) {
			out = append(out, p)
		}
	}
	return CollapseBSDTwins(out), nil
}

func existsNamedPort(device string) bool {
	infos, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return false
	}
	for _, info := range infos {
		if info != nil && info.Name == device {
			return true
		}
	}
	return false
}
