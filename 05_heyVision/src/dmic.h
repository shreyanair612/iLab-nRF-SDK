/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup dmic Audio input control functions
 * @{
 * @ingroup ww_kws
 */

#ifndef __DMIC_H__
#define __DMIC_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DMIC_SAMPLE_BYTES (2)
#define DMIC_PCM_RATE (16000)
#define SAMPLES_BLOCK_LENGTH_MS (10)

int dmic_init(void);
int dmic_start(void);
int dmic_read(void **buffer, size_t *buffer_size, int32_t timeout_ms);
void free_dmic_buffer(void *buffer);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __DMIC_H__ */

/**
 * @}
 */