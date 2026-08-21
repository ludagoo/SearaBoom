#ifndef LISTEN_STATS_H
#define LISTEN_STATS_H

#include <stdbool.h>
#include <stdint.h>

void listen_stats_poll(void);
void listen_stats_persist(void);
bool listen_stats_playing(void);
uint32_t listen_stats_listen_s(void);
uint32_t listen_stats_session_s(void);

#endif
