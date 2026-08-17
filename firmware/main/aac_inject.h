#ifndef AAC_INJECT_H
#define AAC_INJECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_element.h"

typedef struct {
    void (*reset_decoder)(void);
    void (*on_clip)(bool active);
} aac_inject_hooks_t;

audio_element_handle_t aac_inject_init(void);
void aac_inject_deinit(void);
audio_element_handle_t aac_inject_element(void);
void aac_inject_set_hooks(const aac_inject_hooks_t *hooks);
void aac_inject_set_passthrough(bool on);

/* Swap decoder input to this ADTS blob. Non-blocking. gap_ms of I2S underrun silence between loops. */
esp_err_t aac_inject_play(const uint8_t *data, size_t len, bool loop, int gap_ms);
void aac_inject_stop(void);
bool aac_inject_is_playing(void);
bool aac_inject_has_started(void);
esp_err_t aac_inject_wait_done(int timeout_ms);

#endif
