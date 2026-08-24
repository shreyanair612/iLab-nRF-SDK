#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "beamform.h"

LOG_MODULE_REGISTER(beamform, LOG_LEVEL_INF);

#define SAMPLE_RATE 16000
#define CHANNELS    2
#define BLOCK_PAIRS 256
#define BLOCK_BYTES (BLOCK_PAIRS * CHANNELS * sizeof(int16_t))

#define TDM_NODE DT_NODELABEL(tdm)

K_MEM_SLAB_DEFINE(rx_slab, BLOCK_BYTES, 4, 4);
K_THREAD_STACK_DEFINE(capture_stack, 2048);

static const struct device *const i2s = DEVICE_DT_GET(TDM_NODE);

static const struct i2s_config i2s_cfg = {
    .word_size = 16,
    .channels = CHANNELS,
    .format = I2S_FMT_DATA_FORMAT_I2S,
    .options = I2S_OPT_BIT_CLK_CONT | I2S_OPT_FRAME_CLK_MASTER,
    .frame_clk_freq = SAMPLE_RATE,
    .mem_slab = &rx_slab,
    .block_size = BLOCK_BYTES,
    .timeout = 2000,
};

static struct k_thread capture_thread_data;
static atomic_t beamforming_active = ATOMIC_INIT(0);
static void(*chunk_ready)(const int16_t *samples, size_t count);

static void capture(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    int err = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);
    if (err) {
        LOG_ERR("I2S start failed: %d", err);
        atomic_clear(&beamforming_active);
        return;
    }

    while (atomic_get(&beamforming_active)) {
        void *block;
        size_t bytes;
        int16_t mono_chunk[BLOCK_PAIRS];

        int err = i2s_read(i2s, &block, &bytes);
        if(err) {
            if (err == -EAGAIN) {
                LOG_WRN("I2S read failed: %d", err);
            }
            continue;
        }

        int16_t *stereo = block;
        size_t pairs = bytes/ (CHANNELS * sizeof(int16_t));
        pairs = MIN(pairs, (size_t)BLOCK_PAIRS);

        for (size_t i = 0; i < pairs; i++) {
            // beamforming algorithm:
            mono_chunk[i] = ((int32_t)stereo[2*i] + stereo[2*i+1])/2;
        }
       
        if (chunk_ready) {
            chunk_ready(mono_chunk, pairs);
        }

        k_mem_slab_free(&rx_slab, block);
    }

    i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
}

int beamform_init(void (*chunk_cb)(const int16_t *samples, size_t count)) {
    if(!device_is_ready(i2s)) {
        return -ENODEV;
    }

    chunk_ready = chunk_cb;
    return i2s_configure(i2s, I2S_DIR_RX, &i2s_cfg);
}

int beamform_start(void) {
    if(!atomic_cas(&beamforming_active, 0,1)) {
        return -EALREADY;
    }

    k_thread_create(&capture_thread_data, capture_stack, 
        K_THREAD_STACK_SIZEOF(capture_stack), capture, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
    
        return 0;
}