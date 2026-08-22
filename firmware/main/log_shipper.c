#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "log_shipper.h"
#include "config_store.h"
#include "listen_stats.h"
#include "ota_update.h"
#include "searaboom.h"
#include "sdkconfig.h"

static const char *TAG = "log_shipper";

#define LOG_RING_BYTES 8192
#define LOG_POST_MAX 1536
#define LOG_FAIL_BACKOFF_MS 45000
#define LOG_SETTLE_MS 25000
#define LOG_PERIOD_MS 25000
#define LOG_TICK_MS 500
#define LOG_SPILL_PERIOD_MS 10000
#define LOG_FLASH_CAP (48 * 1024)
#define LOG_MU_WAIT_MS 5
#define LOG_TLS_MIN_INTERNAL 8192
#define LOGQ_PATH "/spiffs/logq"
#define LOGQ_TMP "/spiffs/logq.tmp"

/* Large log buffers live in PSRAM. s_chunk is the shipper-task staging
 * buffer (POST text, SPIFFS copy, flash compact) — one 1.5 KB block. */
static EXT_RAM_BSS_ATTR char s_ring[LOG_RING_BYTES];
static size_t s_ring_len;
static SemaphoreHandle_t s_mu;
static vprintf_like_t s_prev_vprintf;
static volatile bool s_paused;
static volatile bool s_inited;
static volatile bool s_wifi_up;
static volatile bool s_settled;
static volatile bool s_flush_req;
static volatile bool s_flush_soon;
static char s_device_id[18];
static char s_fw[32];
static char s_reset[16];
static EXT_RAM_BSS_ATTR char s_chunk[LOG_POST_MAX];
static EXT_RAM_BSS_ATTR char s_payload[LOG_POST_MAX + 768];
static EXT_RAM_BSS_ATTR char s_spill[LOG_POST_MAX];
static size_t s_spill_len;
static size_t s_pending_len;
static bool s_pending_flash;
static bool s_pending_spill;
static bool s_spiffs_ok;
static size_t s_flash_rd;
static size_t s_flash_size;
static uint32_t s_seq;
static int s_last_http = -1;
static int64_t s_wifi_up_ms;

static const char *reset_token(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
#ifdef ESP_RST_USB
    case ESP_RST_USB: return "USB";
#endif
#ifdef ESP_RST_JTAG
    case ESP_RST_JTAG: return "JTAG";
#endif
#ifdef ESP_RST_EFUSE
    case ESP_RST_EFUSE: return "EFUSE";
#endif
#ifdef ESP_RST_PWR_GLITCH
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
#endif
#ifdef ESP_RST_CPU_LOCKUP
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
#endif
    default: return "UNKNOWN";
    }
}

static bool line_is_ew(const char *line, size_t n)
{
    for (size_t i = 0; i + 2 < n && i < 24; i++) {
        char c = line[i];
        if ((c == 'E' || c == 'W') && line[i + 1] == ' ' && line[i + 2] == '(') {
            return true;
        }
    }
    return false;
}

static size_t strip_ansi(char *s, size_t n)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\033' && i + 1 < n && s[i + 1] == '[') {
            i += 2;
            while (i < n && s[i] != 'm') {
                i++;
            }
            continue;
        }
        s[w++] = s[i];
    }
    return w;
}

static bool should_capture(const char *line, size_t n)
{
    const char *p = line;
    const char *end = line + n;
    while (p < end && *p != '(') {
        p++;
    }
    while (p < end && *p != ')') {
        p++;
    }
    if (p < end && *p == ')') {
        p++;
    }
    while (p < end && *p == ' ') {
        p++;
    }
    const char *tag = p;
    while (p < end && *p != ':' && *p != ' ') {
        p++;
    }
    size_t tlen = (size_t)(p - tag);
    static const char *skip[] = {
        "AUDIO_ELEMENT", "AUDIO_PIPELINE", "AUDIO_THREAD", "AUDIO_HAL",
        "AUDIO_EVT", "AUDIO_MEM", "CODEC_ELEMENT_HELPER",
    };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        size_t sl = strlen(skip[i]);
        if (tlen == sl && memcmp(tag, skip[i], sl) == 0) {
            return false;
        }
    }
    return true;
}

static bool want_flash_overflow(void)
{
    return s_paused || !wifi_sta_got_ip();
}

static void preserve_drop(const char *p, size_t drop)
{
    if (drop == 0 || !want_flash_overflow()) {
        return;
    }
    size_t room = sizeof(s_spill) - s_spill_len;
    if (room == 0) {
        return;
    }
    if (drop > room) {
        drop = room;
    }
    memcpy(s_spill + s_spill_len, p, drop);
    s_spill_len += drop;
}

static void ring_append(const char *data, size_t n)
{
    if (!s_mu || n == 0) {
        return;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(LOG_MU_WAIT_MS)) != pdTRUE) {
        return;
    }
    if (n >= LOG_RING_BYTES) {
        data += (n - (LOG_RING_BYTES - 1));
        n = LOG_RING_BYTES - 1;
    }
    while (s_ring_len + n >= LOG_RING_BYTES) {
        char *nl = memchr(s_ring, '\n', s_ring_len);
        size_t drop = nl ? (size_t)(nl - s_ring) + 1 : s_ring_len;
        preserve_drop(s_ring, drop);
        if (drop >= s_ring_len) {
            s_ring_len = 0;
            break;
        }
        memmove(s_ring, s_ring + drop, s_ring_len - drop);
        s_ring_len -= drop;
    }
    memcpy(s_ring + s_ring_len, data, n);
    s_ring_len += n;
    if (line_is_ew(data, n)) {
        s_flush_soon = true;
    }
    if (s_ring_len > (LOG_RING_BYTES * 3) / 4) {
        s_flush_soon = true;
    }
    xSemaphoreGive(s_mu);
}

static int shipper_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t use = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        if (n >= (int)sizeof(line) && use > 0 && line[use - 1] != '\n') {
            line[use - 1] = '\n';
        }
        use = strip_ansi(line, use);
        if (use > 0 && should_capture(line, use)) {
            ring_append(line, use);
        }
    }
    if (s_prev_vprintf) {
        return s_prev_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

static void mount_spiffs(void)
{
    if (esp_spiffs_mounted("storage")) {
        s_spiffs_ok = true;
        return;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_spiffs_ok = true;
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        s_spiffs_ok = false;
    }
}

static void flash_refresh_size(void)
{
    struct stat st;
    if (stat(LOGQ_PATH, &st) == 0) {
        s_flash_size = (size_t)st.st_size;
        if (s_flash_rd > s_flash_size) {
            s_flash_rd = s_flash_size;
        }
    } else {
        s_flash_size = 0;
        s_flash_rd = 0;
    }
}

static void flash_compact(void)
{
    if (!s_spiffs_ok || s_flash_rd == 0) {
        return;
    }
    flash_refresh_size();
    if (s_flash_rd >= s_flash_size) {
        unlink(LOGQ_PATH);
        s_flash_rd = 0;
        s_flash_size = 0;
        return;
    }
    FILE *in = fopen(LOGQ_PATH, "rb");
    if (!in) {
        return;
    }
    FILE *out = fopen(LOGQ_TMP, "wb");
    if (!out) {
        fclose(in);
        return;
    }
    if (fseek(in, (long)s_flash_rd, SEEK_SET) != 0) {
        fclose(in);
        fclose(out);
        unlink(LOGQ_TMP);
        return;
    }
    size_t n;
    while ((n = fread(s_chunk, 1, sizeof(s_chunk), in)) > 0) {
        if (fwrite(s_chunk, 1, n, out) != n) {
            break;
        }
    }
    fclose(in);
    fclose(out);
    unlink(LOGQ_PATH);
    rename(LOGQ_TMP, LOGQ_PATH);
    s_flash_rd = 0;
    flash_refresh_size();
}

static void flash_append(const char *data, size_t n)
{
    if (!s_spiffs_ok || !data || n == 0) {
        return;
    }
    flash_refresh_size();
    size_t unread = s_flash_size - s_flash_rd;
    if (unread + n > LOG_FLASH_CAP) {
        size_t drop = unread + n - LOG_FLASH_CAP;
        s_flash_rd += drop;
        if (s_flash_rd > s_flash_size) {
            s_flash_rd = s_flash_size;
        }
    }
    if (s_flash_rd > 0 && (s_flash_rd >= s_flash_size || s_flash_size + n > LOG_FLASH_CAP)) {
        flash_compact();
    }
    FILE *f = fopen(LOGQ_PATH, "ab");
    if (!f) {
        ESP_LOGW(TAG, "logq open failed");
        return;
    }
    fwrite(data, 1, n, f);
    fclose(f);
    flash_refresh_size();
    if (!wifi_sta_got_ip()) {
        ESP_LOGI(TAG, "spill %u -> logq unread=%u", (unsigned)n,
                 (unsigned)(s_flash_size - s_flash_rd));
    }
}

static size_t flash_unread(void)
{
    return (s_flash_size > s_flash_rd) ? (s_flash_size - s_flash_rd) : 0;
}

static void drain_spill_to_flash(void)
{
    if (s_spill_len == 0 || !s_spiffs_ok || !want_flash_overflow()) {
        return;
    }
    size_t n = 0;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    n = s_spill_len;
    if (n > sizeof(s_chunk)) {
        n = sizeof(s_chunk);
    }
    memcpy(s_chunk, s_spill, n);
    if (n < s_spill_len) {
        memmove(s_spill, s_spill + n, s_spill_len - n);
        s_spill_len -= n;
    } else {
        s_spill_len = 0;
    }
    xSemaphoreGive(s_mu);
    flash_append(s_chunk, n);
}

static void spill_ring_to_flash(void)
{
    if (!s_spiffs_ok || !s_mu) {
        return;
    }
    size_t n = 0;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (s_ring_len == 0) {
        xSemaphoreGive(s_mu);
        return;
    }
    n = s_ring_len;
    if (n > sizeof(s_chunk)) {
        n = sizeof(s_chunk);
        for (size_t i = n; i > 0; --i) {
            if (s_ring[i - 1] == '\n') {
                n = i;
                break;
            }
        }
    }
    memcpy(s_chunk, s_ring, n);
    memmove(s_ring, s_ring + n, s_ring_len - n);
    s_ring_len -= n;
    xSemaphoreGive(s_mu);
    if (n > 0) {
        flash_append(s_chunk, n);
    }
}

static bool peek_flash_chunk(void)
{
    if (flash_unread() == 0) {
        return false;
    }
    FILE *f = fopen(LOGQ_PATH, "rb");
    if (!f) {
        flash_refresh_size();
        return false;
    }
    if (fseek(f, (long)s_flash_rd, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    size_t n = fread(s_chunk, 1, sizeof(s_chunk) - 1, f);
    fclose(f);
    if (n == 0) {
        unlink(LOGQ_PATH);
        s_flash_rd = 0;
        s_flash_size = 0;
        return false;
    }
    bool eof = (s_flash_rd + n >= s_flash_size);
    if (!eof) {
        size_t cut = n;
        while (cut > 0 && s_chunk[cut - 1] != '\n') {
            cut--;
        }
        if (cut > 0) {
            n = cut;
        }
    }
    s_chunk[n] = 0;
    s_pending_len = n;
    s_pending_flash = true;
    s_pending_spill = false;
    return n > 0;
}

static bool take_spill_chunk(void)
{
    if (!s_mu) {
        return false;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (s_spill_len == 0) {
        xSemaphoreGive(s_mu);
        return false;
    }
    size_t n = s_spill_len;
    if (n > sizeof(s_chunk) - 1) {
        n = sizeof(s_chunk) - 1;
        for (size_t i = n; i > 0; --i) {
            if (s_spill[i - 1] == '\n') {
                n = i;
                break;
            }
        }
    }
    memcpy(s_chunk, s_spill, n);
    s_chunk[n] = 0;
    memmove(s_spill, s_spill + n, s_spill_len - n);
    s_spill_len -= n;
    xSemaphoreGive(s_mu);
    s_pending_len = n;
    s_pending_flash = false;
    s_pending_spill = true;
    return n > 0;
}

static void put_back_spill_chunk(void)
{
    if (s_pending_len == 0 || !s_pending_spill || !s_mu) {
        return;
    }
    size_t n = s_pending_len;
    if (n > sizeof(s_spill)) {
        n = sizeof(s_spill);
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    size_t room = sizeof(s_spill) - n;
    if (s_spill_len > room) {
        s_spill_len = room;
    }
    memmove(s_spill + n, s_spill, s_spill_len);
    memcpy(s_spill, s_chunk, n);
    s_spill_len += n;
    xSemaphoreGive(s_mu);
}

static bool take_ring_chunk(void)
{
    if (!s_mu) {
        return false;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (s_ring_len == 0) {
        xSemaphoreGive(s_mu);
        return false;
    }
    size_t n = s_ring_len;
    if (n > sizeof(s_chunk) - 1) {
        n = sizeof(s_chunk) - 1;
        for (size_t i = n; i > 0; --i) {
            if (s_ring[i - 1] == '\n') {
                n = i;
                break;
            }
        }
    }
    memcpy(s_chunk, s_ring, n);
    s_chunk[n] = 0;
    memmove(s_ring, s_ring + n, s_ring_len - n);
    s_ring_len -= n;
    xSemaphoreGive(s_mu);
    s_pending_len = n;
    s_pending_flash = false;
    s_pending_spill = false;
    return n > 0;
}

static void put_back_ring_chunk(void)
{
    if (s_pending_len == 0 || s_pending_flash || s_pending_spill || !s_mu) {
        return;
    }
    size_t n = s_pending_len;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    while (s_ring_len + n >= LOG_RING_BYTES && s_ring_len > 0) {
        size_t end = s_ring_len;
        if (end > 0 && s_ring[end - 1] == '\n') {
            end--;
        }
        while (end > 0 && s_ring[end - 1] != '\n') {
            end--;
        }
        if (end == 0) {
            s_ring_len = 0;
            break;
        }
        s_ring_len = end;
    }
    if (n >= LOG_RING_BYTES) {
        n = LOG_RING_BYTES - 1;
    }
    memmove(s_ring + n, s_ring, s_ring_len);
    memcpy(s_ring, s_chunk, n);
    s_ring_len += n;
    xSemaphoreGive(s_mu);
}

static bool prepare_chunk(void)
{
    s_pending_len = 0;
    s_pending_flash = false;
    s_pending_spill = false;
    if (take_spill_chunk()) {
        return true;
    }
    if (peek_flash_chunk()) {
        return true;
    }
    return take_ring_chunk();
}

static void consume_chunk(void)
{
    if (s_pending_len == 0) {
        return;
    }
    if (s_pending_flash) {
        s_flash_rd += s_pending_len;
        if (s_flash_rd >= s_flash_size) {
            unlink(LOGQ_PATH);
            s_flash_rd = 0;
            s_flash_size = 0;
        } else if (s_flash_rd > 8192) {
            flash_compact();
        }
    }
    s_pending_len = 0;
}

static void revert_chunk(void)
{
    if (s_pending_len == 0) {
        return;
    }
    if (s_pending_spill) {
        put_back_spill_chunk();
    } else if (!s_pending_flash) {
        put_back_ring_chunk();
    }
    s_pending_len = 0;
}

static int get_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static bool ship_chunk(void)
{
    sb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    config_store_load(&cfg);

    uint32_t seq = s_seq + 1;
    int64_t now_ms = esp_timer_get_time() / 1000;
    int off = snprintf(s_payload, sizeof(s_payload),
                       "{\"device_id\":\"%s\",\"fw\":\"%s\",\"reset\":\"%s\","
                       "\"uptime_ms\":%lld,\"heap\":%u,\"rssi\":%d,\"station\":\"%s\","
                       "\"name\":\"%s\",\"city\":\"%s\","
                       "\"playing\":%s,\"listen_s\":%u,\"session_s\":%u,"
                       "\"seq\":%u,\"ts_ms\":%lld,\"text\":",
                       s_device_id, s_fw, s_reset,
                       (long long)now_ms, (unsigned)esp_get_free_heap_size(),
                       get_rssi(), cfg.url_key[0] ? cfg.url_key : "",
                       cfg.name[0] ? cfg.name : "",
                       cfg.city[0] ? cfg.city : "",
                       listen_stats_playing() ? "true" : "false",
                       (unsigned)listen_stats_listen_s(),
                       (unsigned)listen_stats_session_s(),
                       (unsigned)seq, (long long)now_ms);
    if (off < 0 || off >= (int)sizeof(s_payload) - 4) {
        return false;
    }
    s_payload[off++] = '"';
    for (const char *p = s_chunk; *p && off < (int)sizeof(s_payload) - 4; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') {
            s_payload[off++] = '\\';
            s_payload[off++] = c;
        } else if (c == '\n') {
            s_payload[off++] = '\\';
            s_payload[off++] = 'n';
        } else if (c == '\r' || (unsigned char)c < 0x20) {
            continue;
        } else {
            s_payload[off++] = c;
        }
    }
    s_payload[off++] = '"';
    s_payload[off++] = '}';
    s_payload[off] = 0;

    char url[192];
    snprintf(url, sizeof(url), "%s/api/logs", CONFIG_SEARABOOM_LOG_URL);

    bool use_tls = (strncmp(url, "https://", 8) == 0);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
        .buffer_size = 512,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        s_last_http = -1;
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "SearaBoom-LogShipper/1");
    esp_http_client_set_post_field(client, s_payload, strlen(s_payload));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    s_last_http = status;
    bool ok = (err == ESP_OK && status >= 200 && status < 300);
    if (!ok) {
        ESP_LOGW(TAG, "ship fail err=%s http=%d", esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(client);
    if (ok) {
        s_seq = seq;
    }
    return ok;
}

static bool tls_heap_ok(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
           >= LOG_TLS_MIN_INTERNAL;
}

static bool can_post(void)
{
    return s_wifi_up && s_settled && !s_paused && wifi_sta_got_ip()
           && !ota_update_is_busy() && tls_heap_ok();
}

static void shipper_task(void *arg)
{
    (void)arg;
    int64_t last_ship_ms = 0;
    int64_t last_spill_ms = 0;
    int64_t backoff_until_ms = 0;
    bool announced = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LOG_TICK_MS));
        int64_t now = esp_timer_get_time() / 1000;

        drain_spill_to_flash();

        bool sta = wifi_sta_got_ip();
        if (!sta && s_ring_len > LOG_RING_BYTES / 2
            && (now - last_spill_ms) >= LOG_SPILL_PERIOD_MS) {
            spill_ring_to_flash();
            last_spill_ms = now;
        }

        if (s_wifi_up && !s_settled && sta) {
            if ((now - s_wifi_up_ms) >= LOG_SETTLE_MS) {
                s_settled = true;
                s_flush_req = true;
                if (!announced) {
                    ESP_LOGI(TAG, "online");
                    announced = true;
                }
            }
        }

        if (!can_post() || now < backoff_until_ms) {
            continue;
        }

        size_t ring_len = 0;
        if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(5)) == pdTRUE) {
            ring_len = s_ring_len;
            xSemaphoreGive(s_mu);
        }
        bool ring_high = ring_len > (LOG_RING_BYTES * 3) / 4;
        bool due = s_flush_req || s_flush_soon || ring_high
                   || (last_ship_ms == 0)
                   || ((now - last_ship_ms) >= LOG_PERIOD_MS);
        if (!due) {
            continue;
        }

        int sent = 0;
        while (sent < 3 && prepare_chunk()) {
            if (!ship_chunk()) {
                revert_chunk();
                backoff_until_ms = esp_timer_get_time() / 1000 + LOG_FAIL_BACKOFF_MS;
                break;
            }
            consume_chunk();
            sent++;
            if (!s_flush_req && !ring_high && flash_unread() == 0) {
                break;
            }
        }
        if (sent == 0 && flash_unread() == 0 && ring_len == 0) {
            s_chunk[0] = 0;
            s_pending_len = 0;
            s_pending_flash = false;
            s_pending_spill = false;
            if (ship_chunk()) {
                sent = 1;
            } else {
                backoff_until_ms = esp_timer_get_time() / 1000 + LOG_FAIL_BACKOFF_MS;
            }
        }
        if (sent > 0 || (flash_unread() == 0 && ring_len == 0)) {
            s_flush_req = false;
            s_flush_soon = false;
            last_ship_ms = esp_timer_get_time() / 1000;
        }
    }
}

esp_err_t log_shipper_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_fw, app && app->version[0] ? app->version : "unknown", sizeof(s_fw) - 1);
    strncpy(s_reset, reset_token(), sizeof(s_reset) - 1);

    s_prev_vprintf = esp_log_set_vprintf(shipper_vprintf);
    s_paused = false;
    s_inited = true;

    ESP_LOGI(TAG, "boot fw=%s reset=%s", s_fw, s_reset);
    ESP_LOGI(TAG, "PSRAM log buf ring=%u chunk=%u payload=%u spill=%u",
             (unsigned)sizeof(s_ring), (unsigned)sizeof(s_chunk),
             (unsigned)sizeof(s_payload), (unsigned)sizeof(s_spill));

    mount_spiffs();
    flash_refresh_size();

    /* Internal stack: HTTPS + SPIFFS disable the flash cache. */
    if (xTaskCreatePinnedToCore(shipper_task, "log_ship", 12288, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "log shipper task failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Remote logs -> %s/api/logs as %s", CONFIG_SEARABOOM_LOG_URL, s_device_id);
    return ESP_OK;
}

void log_shipper_set_paused(bool paused)
{
    s_paused = paused;
}

bool log_shipper_is_paused(void)
{
    return s_paused;
}

void log_shipper_wifi_up(void)
{
    s_wifi_up = true;
    s_wifi_up_ms = esp_timer_get_time() / 1000;
    s_settled = false;
}

void log_shipper_flush(void)
{
    s_flush_req = true;
}

void log_shipper_logstat(void)
{
    size_t ring = 0;
    size_t spill = 0;
    if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        ring = s_ring_len;
        spill = s_spill_len;
        xSemaphoreGive(s_mu);
    }
    printf("logstat ring=%u flash=%u paused=%d ready=%d seq=%u http=%d\n",
           (unsigned)ring,
           (unsigned)(flash_unread() + spill),
           (int)s_paused,
           (int)(s_inited && s_wifi_up && s_settled),
           (unsigned)s_seq,
           s_last_http);
}
