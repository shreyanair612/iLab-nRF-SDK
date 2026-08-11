#ifndef BEAMFORM_H_
#define BEAMFORM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

int beamform_init(void (*done_cb)(void));
int beamform_start(void);
void beamform_stop(void);

bool beamform_has_audio(void);
const int16_t *beamform_audio(void);
size_t beamform_audio_count(void);
void beamform_clear(void);

#endif