#ifndef BEAMFORM_H_
#define BEAMFORM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

int beamform_init(void (*chunk_cb)(const int16_t *samples, size_t count));
int beamform_start(void);

#endif