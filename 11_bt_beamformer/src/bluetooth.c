#include <errno.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <bluetooth/services/nus.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static struct bt_conn *connection;
static atomic_t subscribed = ATOMIC_INIT(0);
static void (*send_ready)(void);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};
static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void connected(struct bt_conn *conn, uint8_t err) {
    if(!err) {
        connection = bt_conn_ref(conn);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);
    atomic_clear(&subscribed);
    if(connection) {
        bt_conn_unref(connection);
        connection = NULL;
    }
    bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, 
        ARRAY_SIZE(sd));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void nus_send_enabled(enum bt_nus_send_status status) {
    atomic_set(&subscribed, status == BT_NUS_SEND_STATUS_ENABLED);
    if (atomic_get(&subscribed) && send_ready) {
        send_ready();
    }
}

static struct bt_nus_cb nus_cb = {
    .send_enabled = nus_send_enabled,
};

int bluetooth_init(void (*ready_cb)(void)) {
    send_ready = ready_cb;
    int err = bt_enable(NULL);
    if(err) {
        return err;
    }
    err = bt_nus_init(&nus_cb);
    if(err) {
        return err;
    }
    return bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), 
        sd, ARRAY_SIZE(sd));
}

bool bluetooth_ready(void) {
    return connection && atomic_get(&subscribed);
}

int bluetooth_send(const int16_t *audio, size_t samples) {
    if (!bluetooth_ready()) {
        return -ENOTCONN;
    }

    LOG_INF("BLE send: samples=%u, MTU=%u", (uint32_t)samples, bt_gatt_get_mtu(connection));

    uint16_t mtu = bt_gatt_get_mtu(connection);
    size_t samples_per_packet = MAX((size_t)1, (mtu-3) / sizeof(int16_t));

    LOG_INF("BLE send: samples=%u, MTU=%u, samples/packet=%u, packets=%u",
    (uint32_t)samples, mtu, (uint32_t)samples_per_packet, (uint32_t)DIV_ROUND_UP(samples, samples_per_packet));
    
    // send bluetooth packets
    for (size_t offset = 0; offset < samples; offset+= samples_per_packet) {
        size_t count = MIN(samples_per_packet, samples-offset);
        int err = bt_nus_send(NULL, (const uint8_t *)&audio[offset],
            count * sizeof(int16_t));
        if(err) {
            return err;
        }
        k_sleep(K_MSEC(1));
    }
    return 0;
}