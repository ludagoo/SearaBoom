#ifndef BLOB_STREAM_H
#define BLOB_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "audio_element.h"

audio_element_handle_t blob_stream_init(void);
void blob_stream_set_data(audio_element_handle_t el, const uint8_t *data, size_t len);

#endif
