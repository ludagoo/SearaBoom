#ifndef WIFI_BOOT_H
#define WIFI_BOOT_H

#include <stdbool.h>
#include <string.h>

/* SoftAP + ap_welcome policy. Host tests include this header (no ESP types).
 *
 * Welcome is first-setup only (no ssid, no name, no city, station still URL1).
 * A saved network that will not join, or a named box with an empty ssid, must
 * not start SoftAP or play welcome. Explicit wifi wipe sets force_ap so the
 * portal can come back without treating the box as new. */

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

static inline bool sb_wifi_should_start_portal(const char *ssid, const char *name,
                                              const char *city, const char *url_key,
                                              bool force_ap)
{
    return force_ap || sb_wifi_is_first_setup(ssid, name, city, url_key);
}

static inline bool sb_wifi_should_play_welcome(const char *ssid, const char *name,
                                              const char *city, const char *url_key,
                                              bool force_ap)
{
    (void)force_ap;
    return sb_wifi_is_first_setup(ssid, name, city, url_key);
}

#endif
