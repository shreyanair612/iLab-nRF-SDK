#ifndef BLUETOOTH_H_
#define BLUETOOTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*bluetooth_ready_cb_t)(void);

int bluetooth_init(bluetooth_ready_cb_t ready_cb);
bool bluetooth_can_send(void);

int bluetooth_send_pcm(const int16_t *samples, size_t sample_count);

#endif