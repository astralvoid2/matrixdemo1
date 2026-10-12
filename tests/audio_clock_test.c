/* Live, muted WASAPI check: measure device-clock progression across a loop
 * boundary, muted advancement, cleanup and a restart. No audio is recorded. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "audio.h"

int main(void)
{
    audio_set_mute(1);
    if (audio_start_procedural() != 0) {
        fprintf(stderr, "SKIP: no usable Windows playback endpoint\n");
        return 77;
    }
    ULONGLONG start = GetTickCount64();
    double previous = 0.0;
    int failed = 0;
    while (GetTickCount64() - start < 16500) {
        double time = audio_time();
        if (time < previous || !isfinite(time)) failed = 1;
        previous = time;
        Sleep(10);
    }
    double elapsed = (GetTickCount64() - start) / 1000.0;
    double delay = audio_reported_delay();
    if (fabs(previous - elapsed) > 0.3 || previous < 15.0 || delay < 0.0 || delay > 2.0 ||
        audio_visual_lead() != 0.0 || !audio_get_mute()) failed = 1;
    fprintf(stderr, "WASAPI clock %.3f s, elapsed %.3f s, queue %.3f s\n", previous, elapsed, delay);
    audio_stop();
    if (audio_rate() != 0 || audio_time() >= 0.0) failed = 1;
    /* Keep the second stream muted as well. */
    if (audio_start_procedural() != 0) failed = 1;
    else { Sleep(120); if (audio_time() < 0.0 || audio_time() > 1.0) failed = 1; }
    audio_stop();
    fprintf(stderr, "WASAPI clock test: %s\n", failed ? "FAILED" : "PASS");
    return failed ? 1 : 0;
}
