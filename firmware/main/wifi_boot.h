#ifndef WIFI_BOOT_H
#define WIFI_BOOT_H

#include <stdbool.h>
#include <string.h>

/* SoftAP + ap_welcome policy. Host tests include this header (no ESP types).
 *
 * Welcome + SoftAP when there is no home network the box can use:
 * - never configured (first-setup)
 * - empty saved SSID (named box included — same class as a scan miss)
 * - explicit wifi wipe (force_ap)
 * - saved SSID not on the air (ssid_on_air == false)
 *
 * Not welcome / not SoftAP: the saved SSID was seen on scan or was just
 * associated, but DHCP/auth missed the boot window. Stay STA and retry.
 * That was last night's false first-setup (GOOSE! join-fail). */

static inline bool sb_wifi_has_text(const char *s)
{
    return s && s[0] != 0;
}

static inline bool sb_wifi_is_first_setup(const char *ssid, const char *name,
                                         const char *city, const char *url_key)
{
    if (sb_wifi_has_text(ssid) || sb_wifi_has_text(name) || sb_wifi_has_text(city)) {
        return false;
    }
    if (url_key && url_key[0] && strcmp(url_key, "URL1") != 0) {
        return false;
    }
    return true;
}

static inline bool sb_wifi_should_start_portal(const char *ssid, bool force_ap,
                                              bool ssid_on_air)
{
    if (force_ap) {
        return true;
    }
    if (!sb_wifi_has_text(ssid)) {
        return true;
    }
    return !ssid_on_air;
}

static inline bool sb_wifi_should_play_welcome(const char *ssid, bool force_ap,
                                              bool ssid_on_air)
{
    return sb_wifi_should_start_portal(ssid, force_ap, ssid_on_air);
}

#endif
