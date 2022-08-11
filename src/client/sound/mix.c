/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "sound.h"

#define    PAINTBUFFER_SIZE    2048

static int snd_scaletable[32][256];
static int snd_vol;

static void TransferStereo16(samplepair_t *samp, int endtime)
{
    for (int ltime = paintedtime; ltime < endtime;) {
        // handle recirculating buffer issues
        int lpos = ltime & ((dma.samples >> 1) - 1);
        int count = (dma.samples >> 1) - lpos;
        if (ltime + count > endtime)
            count = endtime - ltime;

        // write a linear blast of samples
        int16_t *out = (int16_t *)dma.buffer + (lpos << 1);
        for (int i = 0; i < count; i++, samp++, out += 2) {
            int left = samp->left >> 8;
            int right = samp->right >> 8;
            out[0] = clamp(left, INT16_MIN, INT16_MAX);
            out[1] = clamp(right, INT16_MIN, INT16_MAX);
        }

        ltime += count;
    }
}

static void TransferStereo(samplepair_t *samp, int endtime)
{
    int *p = (int *)samp;
    int count = (endtime - paintedtime) * dma.channels;
    int out_mask = dma.samples - 1;
    int out_idx = paintedtime * dma.channels & out_mask;
    int step = 3 - dma.channels;
    int val;

    if (dma.samplebits == 16) {
        int16_t *out = (int16_t *)dma.buffer;
        while (count--) {
            val = *p >> 8;
            p += step;
            clamp(val, INT16_MIN, INT16_MAX);
            out[out_idx] = val;
            out_idx = (out_idx + 1) & out_mask;
        }
    } else if (dma.samplebits == 8) {
        uint8_t *out = (uint8_t *)dma.buffer;
        while (count--) {
            val = *p >> 8;
            p += step;
            clamp(val, INT16_MIN, INT16_MAX);
            out[out_idx] = (val >> 8) + 128;
            out_idx = (out_idx + 1) & out_mask;
        }
    }
}

static void TransferPaintBuffer(samplepair_t *samp, int endtime)
{
    if (s_testsound->integer) {
        int i;

        // write a fixed sine wave
        for (i = paintedtime; i < endtime; i++) {
            samp[i].left = samp[i].right = sin(i * 0.1f) * 20000 * 256;
        }
    }

    if (dma.samplebits == 16 && dma.channels == 2) {
        // optimized case
        TransferStereo16(samp, endtime);
    } else {
        // general case
        TransferStereo(samp, endtime);
    }
}


/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

typedef void (*paintfunc_t)(channel_t *, sfxcache_t *, int, samplepair_t *);

#define PAINTFUNC(name) \
    static void name(channel_t *ch, sfxcache_t *sc, int count, samplepair_t *samp)

PAINTFUNC(PaintMono8)
{
    int *lscale = snd_scaletable[ch->leftvol >> 3];
    int *rscale = snd_scaletable[ch->rightvol >> 3];
    uint8_t *sfx = (uint8_t *)sc->data + ch->pos;

    for (int i = 0; i < count; i++, samp++, sfx++) {
        samp->left += lscale[*sfx];
        samp->right += rscale[*sfx];
    }
}

PAINTFUNC(PaintStereo8)
{
    int vol = ch->master_vol * 255;
    int *scale = snd_scaletable[vol >> 3];
    uint8_t *sfx = (uint8_t *)sc->data + ch->pos * 2;

    for (int i = 0; i < count; i++, samp++, sfx += 2) {
        samp->left += scale[sfx[0]];
        samp->right += scale[sfx[1]];
    }
}

PAINTFUNC(PaintMono16)
{
    int leftvol = ch->leftvol * snd_vol;
    int rightvol = ch->rightvol * snd_vol;
    int16_t *sfx = (int16_t *)sc->data + ch->pos;

    for (int i = 0; i < count; i++, samp++, sfx++) {
        samp->left += (*sfx * leftvol) >> 8;
        samp->right += (*sfx * rightvol) >> 8;
    }
}

PAINTFUNC(PaintStereo16)
{
    int vol = ch->master_vol * 255 * snd_vol;
    int16_t *sfx = (int16_t *)sc->data + ch->pos * 2;

    for (int i = 0; i < count; i++, samp++, sfx += 2) {
        samp->left += (sfx[0] * vol) >> 8;
        samp->right += (sfx[1] * vol) >> 8;
    }
}

static const paintfunc_t paintfuncs[] = {
    PaintMono8,
    PaintStereo8,
    PaintMono16,
    PaintStereo16
};

void S_PaintChannels(int endtime)
{
    samplepair_t paintbuffer[PAINTBUFFER_SIZE];
    int i;
    int end;
    channel_t *ch;
    sfxcache_t *sc;
    int ltime, count;
    playsound_t *ps;

    while (paintedtime < endtime) {
        // if paintbuffer is smaller than DMA buffer
        end = endtime;
        if (end - paintedtime > PAINTBUFFER_SIZE)
            end = paintedtime + PAINTBUFFER_SIZE;

        // start any playsounds
        while (1) {
            ps = s_pendingplays.next;
            if (ps == &s_pendingplays)
                break;    // no more pending sounds
            if (ps->begin <= paintedtime) {
                S_IssuePlaysound(ps);
                continue;
            }

            if (ps->begin < end)
                end = ps->begin;        // stop here
            break;
        }

        // clear the paint buffer
        memset(paintbuffer, 0, (end - paintedtime) * sizeof(samplepair_t));

        // paint in the channels.
        ch = channels;
        for (i = 0; i < s_numchannels; i++, ch++) {
            ltime = paintedtime;

            while (ltime < end) {
                if (!ch->sfx || (!ch->leftvol && !ch->rightvol))
                    break;

                // max painting is to the end of the buffer
                count = end - ltime;

                // might be stopped by running out of data
                if (ch->end - ltime < count)
                    count = ch->end - ltime;

                sc = S_LoadSound(ch->sfx);
                if (!sc)
                    break;

                if (count > 0) {
                    int func = (sc->width - 1) * 2 + (sc->channels - 1);
                    paintfuncs[func](ch, sc, count, &paintbuffer[ltime - paintedtime]);
                    ch->pos += count;
                    ltime += count;
                }

                // if at end of loop, restart
                if (ltime >= ch->end) {
                    if (ch->autosound) {
                        // autolooping sounds always go back to start
                        ch->pos = 0;
                        ch->end = ltime + sc->length;
                    } else if (sc->loopstart >= 0) {
                        ch->pos = sc->loopstart;
                        ch->end = ltime + sc->length - ch->pos;
                    } else {
                        // channel just stopped
                        ch->sfx = NULL;
                    }
                }
            }
        }

        // transfer out according to DMA format
        TransferPaintBuffer(paintbuffer, end);
        paintedtime = end;
    }
}

void S_InitScaletable(void)
{
    snd_vol = Cvar_ClampValue(s_volume, 0, 1) * 256;

    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 256; j++)
            snd_scaletable[i][j] = (j - 128) * i * 8 * snd_vol;

    s_volume->modified = false;
}
