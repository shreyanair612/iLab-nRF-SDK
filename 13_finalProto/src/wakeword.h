/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __WAKEWORD_H__
#define __WAKEWORD_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ww_init(void);
void ww_reset(void);
int ww_process(const int16_t *const samples, const uint16_t num_samples, bool *const ww_detected);

#ifdef __cplusplus
}
#endif

#endif /* __WAKEWORD_H__ */