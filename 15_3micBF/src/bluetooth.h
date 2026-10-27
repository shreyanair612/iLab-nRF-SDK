#ifndef BLUETOOTH_H_
#define BLUETOOTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int bluetooth_init(void);
int bluetooth_enqueue_audio(const int16_t *samples, size_t count);
bool bluetooth_tx_drained(void);

/* Chunks lost since the last bluetooth_tx_reset_stats(), either because the
 * TX queue was full or because the link was down. Any non-zero value means
 * the received audio has gaps. */
uint32_t bluetooth_tx_dropped_chunks(void);
void bluetooth_tx_reset_stats(void);

#endif