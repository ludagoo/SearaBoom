#include <string.h>
#include "blob_stream.h"
#include "audio_element.h"
#include "audio_mem.h"

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} blob_stream_t;

static esp_err_t blob_open(audio_element_handle_t self)
{
    blob_stream_t *b = audio_element_getdata(self);
    b->pos = 0;
    return ESP_OK;
}

static int blob_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    blob_stream_t *b = audio_element_getdata(self);
    if (!b->data || b->pos >= b->len || in_len <= 0) {
        return AEL_IO_OK;
    }
    int n = in_len;
    if ((size_t)n > b->len - b->pos) {
        n = (int)(b->len - b->pos);
    }
    memcpy(in_buffer, b->data + b->pos, (size_t)n);
    b->pos += (size_t)n;
    return audio_element_output(self, in_buffer, n);
}

static esp_err_t blob_close(audio_element_handle_t self)
{
    blob_stream_t *b = audio_element_getdata(self);
    b->pos = 0;
    return ESP_OK;
}

static esp_err_t blob_destroy(audio_element_handle_t self)
{
    audio_free(audio_element_getdata(self));
    return ESP_OK;
}

audio_element_handle_t blob_stream_init(void)
{
    blob_stream_t *b = audio_calloc(1, sizeof(*b));
    if (!b) {
        return NULL;
    }
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = blob_open;
    cfg.process = blob_process;
    cfg.close = blob_close;
    cfg.destroy = blob_destroy;
    cfg.tag = "blob";
    cfg.out_rb_size = 8 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 6;
    cfg.task_core = 1;
    cfg.stack_in_ext = true;
    cfg.buffer_len = 2048;
    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        audio_free(b);
        return NULL;
    }
    audio_element_setdata(el, b);
    return el;
}

void blob_stream_set_data(audio_element_handle_t el, const uint8_t *data, size_t len)
{
    blob_stream_t *b = audio_element_getdata(el);
    if (!b) {
        return;
    }
    b->data = data;
    b->len = len;
    b->pos = 0;
}
