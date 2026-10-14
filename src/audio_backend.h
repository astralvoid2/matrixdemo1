#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H
#include <stdint.h>
/* Takes ownership of samples on both success and failure. */
int audio_backend_start(int16_t *samples, long frames, unsigned rate);
#endif
