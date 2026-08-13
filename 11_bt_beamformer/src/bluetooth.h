#ifndef BLUETOOTH_H_
#define BLUETOOTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int bluetooth_init(void);
int bluetooth_enqueue_audio(const int16_t *samples, size_t count);
bool bluetooth_tx_drained(void);

#endif