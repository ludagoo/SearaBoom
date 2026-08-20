#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "searaboom.h"

/* Setup AP. Returns after save once the phone leaves, with STA on home Wi-Fi. */
void captive_portal_run(void);

#endif
