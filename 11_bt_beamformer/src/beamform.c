#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(beamform);

#define SAMPLE_RATE 16000
#define CHANNELS    2
#define BLOCK_PAIRS 256
#define BLOCK_BYTES (BLOCK_PAIRS * CHANNELS * sizeof(int16_t))
#define MAX_SAMPLES (SAMPLE_RATE * 8) 

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

static int16_t mono[MAX_SAMPLES];
static size_t mono_count;
static atomic_t recording = ATOMIC_INIT(0);
static atomic_t audio_ready = ATOMIC_INIT(0);
static struct k_thread capture_thread_data;
static void (*recording_done)(void);

static void capture(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    if(i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START)) {
        atomic_clear(&recording);
        return;
    }

    while (atomic_get(&recording) && mono_count < MAX_SAMPLES) {
        void *block;
        size_t bytes;
        int err = i2s_read(i2s, &block, &bytes);

        if(err) {
            if (err == -EAGAIN) {
                continue;
            }
            break;
        }

        int16_t *stereo = block;
        size_t pairs = bytes/ (CHANNELS * sizeof(int16_t));
        for (size_t i =0; i < pairs && mono_count < MAX_SAMPLES; i++) {
            // beamforming algorithm:
            mono[mono_count++] = ((int32_t)stereo[2*i] + stereo[2*i+1])/2;
        }
        k_mem_slab_free(&rx_slab, block);
    }

    i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
    atomic_clear(&recording);
    atomic_set(&audio_ready, 1);
    if(recording_done) {
        recording_done();
    }
}

int beamform_init(void (*done_cb)(void)) {
    recording_done = done_cb;
    if(!device_is_ready(i2s)) {
        return -ENODEV;
    }
    return i2s_configure(i2s, I2S_DIR_RX, &i2s_cfg);
}

int beamform_start(void) {
    if(atomic_get(&recording) || atomic_get(&audio_ready)) {
        return -EBUSY;
    }

    mono_count = 0;
    atomic_set(&recording,1);
    k_thread_create(&capture_thread_data, capture_stack, 
        K_THREAD_STACK_SIZEOF(capture_stack), capture, NULL, NULL, NULL,
        6, 0, K_NO_WAIT);
    return 0;
}

void beamform_stop(void) {
    atomic_clear(&recording);
    i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
}

bool beamform_has_audio(void) {
    return atomic_get(&audio_ready);
}

const int16_t *beamform_audio(void) {
    return mono;
}

size_t beamform_audio_count(void) {
    return mono_count;
}

void beamform_clear(void) {
    mono_count = 0;
    atomic_clear(&audio_ready);
}