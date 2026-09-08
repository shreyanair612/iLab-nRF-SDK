#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <bluetooth/services/nus.h>

#include "bluetooth.h"

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define AUDIO_CHUNK_SAMPLES 256
#define AUDIO_QUEUE_DEPTH 8

struct audio_chunk {
    size_t samples;
    int16_t pcm[AUDIO_CHUNK_SAMPLES];
};

K_MSGQ_DEFINE(audio_tx_queue, sizeof(struct audio_chunk), AUDIO_QUEUE_DEPTH, 4);
K_THREAD_STACK_DEFINE(ble_tx_stack, 2048);

/* Longest a queued chunk waits for the link to come back before it is dropped.
 * Bounded so STATE_FINALIZE can always drain and return the app to idle, even
 * if the central disconnects mid-utterance. */
#define TX_LINK_WAIT_MS 500

static struct bt_conn *connection;
static atomic_t subscribed = ATOMIC_INIT(0);
/*
 * Chunks accepted by bluetooth_enqueue_audio() but not yet fully sent. This
 * counts the one being transmitted as well as those still in the queue, so
 * bluetooth_tx_drained() has no window where an in-flight chunk looks idle.
 */
static atomic_t tx_pending = ATOMIC_INIT(0);
static atomic_t tx_dropped = ATOMIC_INIT(0);
static struct k_thread ble_tx_thread_data;

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};
static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static bool bluetooth_ready(void) {
    return connection != NULL && atomic_get(&subscribed);
}

static void connected(struct bt_conn *conn, uint8_t err) {
    if(err) {
        LOG_WRN("BLE connection failed: %u", err);
        return;
    }

    connection = bt_conn_ref(conn);
    LOG_INF("BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);

    atomic_clear(&subscribed);

    if(connection) {
        bt_conn_unref(connection);
        connection = NULL;
    }

    LOG_INF("BLE disconnected");
    bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, 
        ARRAY_SIZE(sd));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void nus_send_enabled(enum bt_nus_send_status status) {
    atomic_set(&subscribed, status == BT_NUS_SEND_STATUS_ENABLED);
    LOG_INF("NUS notifications %s", atomic_get(&subscribed) ? "enabled" : "disabled");
}

static struct bt_nus_cb nus_cb = {
    .send_enabled = nus_send_enabled,
};

static int send_chunk(const struct audio_chunk *chunk) {
    if (!bluetooth_ready()) {
        return -ENOTCONN;
    }

    uint16_t mtu = bt_gatt_get_mtu(connection);
    size_t samples_per_packet = MAX((size_t)1, (mtu-3) / sizeof(int16_t));

    for(size_t offset = 0; offset < chunk->samples; offset += samples_per_packet) {
        size_t count = MIN(samples_per_packet, chunk->samples - offset);
        int err = bt_nus_send(NULL, (const uint8_t *)&chunk->pcm[offset], count*sizeof(int16_t));

        if(err) return err;

        k_sleep(K_MSEC(1));
    }

    return 0;
}


static void ble_tx_worker(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while(true) {
        struct audio_chunk chunk;
        int err = k_msgq_get(&audio_tx_queue, &chunk, K_FOREVER);
        if (err) continue;

        for (int waited = 0; !bluetooth_ready() && waited < TX_LINK_WAIT_MS; waited += 20) {
            k_sleep(K_MSEC(20));
        }

        if (bluetooth_ready()) {
            err = send_chunk(&chunk);
            if (err) {
                atomic_inc(&tx_dropped);
                LOG_WRN("BLE send failed; dropped one chunk: %d", err);
            }
        } else {
            atomic_inc(&tx_dropped);
            LOG_WRN("BLE link down; dropped one chunk");
        }

        atomic_dec(&tx_pending);
    }
}

int bluetooth_init(void) {
    int err = bt_enable(NULL);
    if(err) {
        return err;
    }

    err = bt_nus_init(&nus_cb);
    if(err) {
        return err;
    }

    k_thread_create(&ble_tx_thread_data, ble_tx_stack, 
        K_THREAD_STACK_SIZEOF(ble_tx_stack), ble_tx_worker,
        NULL, NULL, NULL, 7, 0, K_NO_WAIT);

    return bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), 
        sd, ARRAY_SIZE(sd));
}

int bluetooth_enqueue_audio(const int16_t *samples, size_t count) {
    if(count==0 || count > AUDIO_CHUNK_SAMPLES) {
        return -EINVAL;
    }

    if(!bluetooth_ready()) {
        return -ENOTCONN;
    }

    struct audio_chunk chunk = {
        .samples = count,
    };

    memcpy(chunk.pcm, samples, count*sizeof(int16_t));

    /*
     * Claim the slot before the put so the counter can never lag behind the
     * queue. Released by the TX worker once the chunk has actually been sent.
     */
    atomic_inc(&tx_pending);

    int err = k_msgq_put(&audio_tx_queue, &chunk, K_NO_WAIT);

    if (err) {
        atomic_dec(&tx_pending);
        /* Queue full: the link is not keeping up and this frame is lost,
         * which is audible as a gap. Surface it rather than hiding it. */
        atomic_inc(&tx_dropped);
    }

    return err;
}

bool bluetooth_tx_drained(void) {
    return atomic_get(&tx_pending) == 0;
}

uint32_t bluetooth_tx_dropped_chunks(void) {
    return (uint32_t)atomic_get(&tx_dropped);
}

void bluetooth_tx_reset_stats(void) {
    atomic_clear(&tx_dropped);
}