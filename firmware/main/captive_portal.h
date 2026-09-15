#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "searaboom.h"

/* Setup AP. Returns after save once the phone leaves, with STA on home Wi-Fi.
 * play_welcome: first-setup only. Reconfig (wifi wipe) stays quiet. */
void captive_portal_run(bool play_welcome);

#endif
