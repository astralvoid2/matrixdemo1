/* Native Windows playback. All COM/audio objects live on the worker thread.
 * The renderer reads a protected snapshot of IAudioClock's device position;
 * it does not use the count of samples merely submitted for playback. */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <process.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "audio_backend.h"

static const CLSID demo_clsid_enumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const IID demo_iid_enumerator = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const IID demo_iid_client = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const IID demo_iid_render = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const IID demo_iid_clock = {0xcd63314f,0x3fba,0x4a1b,{0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7}};

static int16_t *g_samples;
static long g_frames;
static unsigned g_rate;
static HANDLE g_thread, g_stop, g_ready;
static HRESULT g_start_result;
static volatile LONG g_running, g_mute;
static SRWLOCK g_clock_lock = SRWLOCK_INIT;
static double g_played, g_stamp, g_delay, g_delay_ema;
static int g_clock_valid;
static double g_last_time; /* only accessed by the render thread */

static double now_sec(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static HRESULT fill_buffer(IAudioRenderClient *render, UINT32 frames, long *position)
{
    if (!frames) return S_OK;
    BYTE *data = NULL;
    HRESULT hr = IAudioRenderClient_GetBuffer(render, frames, &data);
    if (FAILED(hr)) return hr;
    int mute = InterlockedCompareExchange(&g_mute, 0, 0) != 0;
    if (!mute) {
        int16_t *out = (int16_t *)data;
        for (UINT32 i = 0; i < frames; i++) {
            size_t input = (size_t)*position * 2;
            out[(size_t)i * 2] = g_samples[input];
            out[(size_t)i * 2 + 1] = g_samples[input + 1];
            if (++*position == g_frames) *position = 0;
        }
    } else {
        *position = (long)(((uint64_t)*position + frames) % (uint64_t)g_frames);
    }
    return IAudioRenderClient_ReleaseBuffer(render, frames, mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
}

static void update_clock(IAudioClient *client, IAudioClock *clock, UINT64 frequency)
{
    UINT64 position = 0, qpc_100ns = 0;
    UINT32 padding = 0;
    if (FAILED(IAudioClock_GetPosition(clock, &position, &qpc_100ns)) ||
        FAILED(IAudioClient_GetCurrentPadding(client, &padding))) return;
    double stamp = qpc_100ns ? (double)qpc_100ns * 1e-7 : now_sec();
    double delay = (double)padding / g_rate;
    AcquireSRWLockExclusive(&g_clock_lock);
    g_played = (double)position / (double)frequency;
    g_stamp = stamp;
    g_delay = delay;
    g_delay_ema = g_delay_ema < 0.0 ? delay : g_delay_ema + 0.02 * (delay - g_delay_ema);
    g_clock_valid = 1;
    ReleaseSRWLockExclusive(&g_clock_lock);
}

static unsigned __stdcall playback_thread(void *unused)
{
    (void)unused;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    IAudioClock *clock = NULL;
    HANDLE audio_event = NULL, scheduling = NULL;
    DWORD task_index = 0;
    UINT32 buffer_frames = 0;
    UINT64 frequency = 0;
    long position = 0;
    int com_initialized = 0, started = 0, ready_sent = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) goto done;
    com_initialized = 1;
    hr = CoCreateInstance(&demo_clsid_enumerator, NULL, CLSCTX_ALL,
                          &demo_iid_enumerator, (void **)&enumerator);
    if (FAILED(hr)) goto done;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) goto done;
    hr = IMMDevice_Activate(device, &demo_iid_client, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr)) goto done;

    WAVEFORMATEX format = {0};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = g_rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = g_rate * 4;
    /* Shared mode mixes alongside other applications. Windows converts our
     * stereo 16-bit samples to the endpoint's native format/sample rate. */
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 600000, 0, &format, NULL);
    if (FAILED(hr)) goto done;
    audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!audio_event) { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    hr = IAudioClient_SetEventHandle(client, audio_event);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetBufferSize(client, &buffer_frames);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetService(client, &demo_iid_render, (void **)&render);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetService(client, &demo_iid_clock, (void **)&clock);
    if (FAILED(hr)) goto done;
    hr = IAudioClock_GetFrequency(clock, &frequency);
    if (FAILED(hr) || !frequency) { if (SUCCEEDED(hr)) hr = E_FAIL; goto done; }
    hr = fill_buffer(render, buffer_frames, &position);
    if (FAILED(hr)) goto done;
    scheduling = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) goto done;
    started = 1;
    InterlockedExchange(&g_running, 1);
    update_clock(client, clock, frequency);
    g_start_result = S_OK;
    SetEvent(g_ready);
    ready_sent = 1;

    HANDLE events[2] = {g_stop, audio_event};
    for (;;) {
        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_TIMEOUT) {
            /* A stalled or unplugged endpoint must not advance visuals forever. */
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        }
        if (wait_result != WAIT_OBJECT_0 + 1) { hr = E_FAIL; break; }
        UINT32 padding = 0;
        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr)) break;
        if (padding > buffer_frames) { hr = E_FAIL; break; }
        hr = fill_buffer(render, buffer_frames - padding, &position);
        if (FAILED(hr)) break;
        update_clock(client, clock, frequency);
    }

done:
    if (FAILED(hr)) fprintf(stderr, "audio: WASAPI error 0x%08lx%s\n", (unsigned long)hr,
                            ready_sent ? " (restart to select the current output)" : "");
    if (started) IAudioClient_Stop(client);
    if (scheduling) AvRevertMmThreadCharacteristics(scheduling);
    if (clock) IAudioClock_Release(clock);
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (audio_event) CloseHandle(audio_event);
    if (com_initialized) CoUninitialize();
    InterlockedExchange(&g_running, 0);
    if (!ready_sent) { g_start_result = hr; SetEvent(g_ready); }
    return 0;
}

int audio_backend_start(int16_t *samples, long frames, unsigned rate)
{
    audio_stop();
    if (!samples || frames <= 0 || !rate || rate > UINT32_MAX / 4) { free(samples); return -1; }
    g_samples = samples;
    g_frames = frames;
    g_rate = rate;
    g_last_time = 0.0;
    AcquireSRWLockExclusive(&g_clock_lock);
    g_clock_valid = 0;
    g_played = g_stamp = g_delay = 0.0;
    g_delay_ema = -1.0;
    ReleaseSRWLockExclusive(&g_clock_lock);
    g_start_result = E_FAIL;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop || !g_ready) goto fail;
    g_thread = (HANDLE)_beginthreadex(NULL, 0, playback_thread, NULL, 0, NULL);
    if (!g_thread) goto fail;
    if (WaitForSingleObject(g_ready, INFINITE) != WAIT_OBJECT_0 || FAILED(g_start_result)) goto fail;
    fprintf(stderr, "audio: playing %.2f s loop at %u Hz via WASAPI shared\n", (double)frames / rate, rate);
    return 0;
fail:
    audio_stop();
    return -1;
}

double audio_time(void)
{
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return -1.0;
    AcquireSRWLockShared(&g_clock_lock);
    double played = g_played, stamp = g_stamp;
    int valid = g_clock_valid;
    ReleaseSRWLockShared(&g_clock_lock);
    if (!valid) return 0.0;
    double elapsed = fmax(0.0, fmin(now_sec() - stamp, 0.1));
    double result = fmax(g_last_time, played + elapsed);
    g_last_time = result;
    return result;
}

double audio_reported_delay(void)
{
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return -1.0;
    AcquireSRWLockShared(&g_clock_lock);
    double delay = g_delay;
    ReleaseSRWLockShared(&g_clock_lock);
    return delay;
}

double audio_smoothed_delay(void)
{
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return 0.0;
    AcquireSRWLockShared(&g_clock_lock);
    double delay = g_delay_ema < 0.0 ? 0.0 : g_delay_ema;
    ReleaseSRWLockShared(&g_clock_lock);
    return delay;
}

double audio_visual_lead(void) { return 0.0; }
const char *audio_backend_name(void) { return audio_rate() ? "WASAPI shared" : "off"; }
unsigned audio_rate(void) { return InterlockedCompareExchange(&g_running, 0, 0) ? g_rate : 0; }
void audio_set_mute(int mute) { InterlockedExchange(&g_mute, mute != 0); }
int audio_get_mute(void) { return InterlockedCompareExchange(&g_mute, 0, 0) != 0; }

void audio_stop(void)
{
    if (g_stop) SetEvent(g_stop);
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
    if (g_stop) { CloseHandle(g_stop); g_stop = NULL; }
    if (g_ready) { CloseHandle(g_ready); g_ready = NULL; }
    InterlockedExchange(&g_running, 0);
    free(g_samples);
    g_samples = NULL;
    g_rate = 0;
}
