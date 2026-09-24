#ifndef PCM_UPMIX_H
#define PCM_UPMIX_H

#include "audio_element.h"

/* Output 44.1 kHz stereo. Waits for channel count; resamples if needed. */
#define PCM_UPMIX_OUT_RB_SIZE (16 * 1024)

audio_element_handle_t pcm_upmix_init(const char *tag);
audio_element_handle_t pcm_upmix_init_core(const char *tag, int core);
/* Last decoded block peak for the radio upmix (`rm2s`). Clip upmix is ignored. */
int pcm_upmix_radio_peak(void);

#endif
