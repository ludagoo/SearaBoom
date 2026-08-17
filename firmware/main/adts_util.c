#include "adts_util.h"

int sb_adts_duration_ms(const uint8_t *p, size_t n)
{
    static const int sr_tab[16] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, 0
    };
    size_t i = 0;
    int frames = 0;
    int sr = 22050;
    if (!p || n < 7) {
        return 4000;
    }
    while (i + 7 <= n) {
        if (p[i] != 0xFF || (p[i + 1] & 0xF0) != 0xF0) {
            i++;
            continue;
        }
        int sidx = (p[i + 2] >> 2) & 0x0F;
        if (sr_tab[sidx]) {
            sr = sr_tab[sidx];
        }
        int flen = ((p[i + 3] & 0x03) << 11) | (p[i + 4] << 3) | ((p[i + 5] >> 5) & 0x07);
        if (flen < 7 || i + (size_t)flen > n) {
            break;
        }
        frames++;
        i += (size_t)flen;
    }
    if (frames <= 0 || sr <= 0) {
        return 4000;
    }
    return (int)((frames * 1024LL * 1000LL) / sr);
}
