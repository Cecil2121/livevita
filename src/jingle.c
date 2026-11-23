/* LiveVita: a short startup jingle. Plays app0:jingle.raw (48 kHz, 16-bit stereo PCM)
 * once on its own thread, so it never blocks the UI. Silently does nothing if the file
 * or an audio port isn't available. */
#include "jingle.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#define JINGLE_RATE   48000
#define JINGLE_GRAIN  1024                  /* stereo frames per output call (matches the file's padding) */

static int jingle_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    FILE *f = fopen("app0:jingle.raw", "rb");
    if (!f) return 0;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, JINGLE_GRAIN, JINGLE_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { fclose(f); return 0; }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    short buf[JINGLE_GRAIN * 2];             /* one grain of interleaved stereo */
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (n < sizeof buf) memset((char *)buf + n, 0, sizeof buf - n);   /* pad the final short chunk with silence */
        sceAudioOutOutput(port, buf);        /* blocks until the grain has played */
    }
    sceAudioOutReleasePort(port);
    fclose(f);
    return 0;
}

void jingle_play(void)
{
    SceUID th = sceKernelCreateThread("livevita_jingle", jingle_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
