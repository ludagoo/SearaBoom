#ifndef ADTS_UTIL_H
#define ADTS_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Duration of an ADTS AAC blob. Returns 4000 if the stream cannot be parsed. */
int sb_adts_duration_ms(const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif
