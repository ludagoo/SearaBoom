#ifndef WIFI_BOOT_H
#define WIFI_BOOT_H

#include <stdbool.h>
#include <string.h>

/* SoftAP + ap_welcome policy. Host tests include this header (no ESP types).
 *
 * Welcome + SoftAP when there is no home network the box can use:
 * - never configured (first-setup)
 * - empty saved SSID (named box included)
 * - explicit wifi wipe (force_ap)
 * - a *successful* probe of the saved SSID that returns zero APs (gone)
 *
 * Not welcome / not SoftAP:
 * - saved SSID seen on a directed probe, or just associated, but DHCP/auth
 *   missed the boot window (last night's GOOSE! join-fail)
 * - scan failed, still connecting, or otherwise indeterminate — fail closed
 *   toward STA retry, never SoftAP + welcome
 */

typedef enum {
    SB_WIFI_AIR_GONE = 0,
    SB_WIFI_AIR_SEEN = 1,
    SB_WIFI_AIR_UNKNOWN = 2,
} sb_wifi_air_t;

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
                                              sb_wifi_air_t air)
{
    if (force_ap) {
        return true;
    }
    if (!sb_wifi_has_text(ssid)) {
        return true;
    }
    return air == SB_WIFI_AIR_GONE;
}

static inline bool sb_wifi_should_play_welcome(const char *ssid, bool force_ap,
                                              sb_wifi_air_t air)
{
    return sb_wifi_should_start_portal(ssid, force_ap, air);
}

#endif
