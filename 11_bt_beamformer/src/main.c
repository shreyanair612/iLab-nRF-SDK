/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Nordic UART Bridge Service (NUS) sample
 */
#include <uart_async_adapter.h>

#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <soc.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>

#include <bluetooth/services/nus.h>

#include <dk_buttons_and_leds.h>

#include <zephyr/settings/settings.h>

#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>

/* Includes for 2MIC I2S DNS Beamformer */
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>

#define LOG_MODULE_NAME peripheral_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define STACKSIZE CONFIG_BT_NUS_THREAD_STACK_SIZE
#define PRIORITY 7

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	(sizeof(DEVICE_NAME) - 1)

#define RUN_STATUS_LED DK_LED1
#define RUN_LED_BLINK_INTERVAL 1000

#define CON_STATUS_LED DK_LED2

#define KEY_PASSKEY_ACCEPT DK_BTN1_MSK
#define KEY_PASSKEY_REJECT DK_BTN2_MSK

/* Beamformer Control Buttons */ 
#define RECORD_START_BTN_MSK DK_BTN1_MSK
#define RECORD_STOP_BTN_MSK DK_BTN2_MSK

#define UART_BUF_SIZE CONFIG_BT_NUS_UART_BUFFER_SIZE
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(50)
#define UART_WAIT_FOR_RX CONFIG_BT_NUS_UART_RX_WAIT_TIME

static K_SEM_DEFINE(ble_init_ok, 0, 1);

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;
static struct k_work adv_work;

#if DT_HAS_CHOSEN(nordic_nus_uart)
#define NUS_UART_NODE DT_CHOSEN(nordic_nus_uart)
#elif DT_HAS_CHOSEN(zephyr_shell_uart)
#define NUS_UART_NODE DT_CHOSEN(zephyr_shell_uart)
#else
#error "No NUS UART: add chosen 'nordic,nus-uart' to devicetree (e.g. in a board overlay)."
#endif
static const struct device *uart = DEVICE_DT_GET(NUS_UART_NODE);
static struct k_work_delayable uart_work;

struct uart_data_t {
	void *fifo_reserved;
	uint8_t data[UART_BUF_SIZE];
	uint16_t len;
};

static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

#ifdef CONFIG_UART_ASYNC_ADAPTER
UART_ASYNC_ADAPTER_INST_DEFINE(async_adapter);
#else
#define async_adapter NULL
#endif

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);

	static size_t aborted_len;
	struct uart_data_t *buf;
	static uint8_t *aborted_buf;
	static bool disable_req;

	switch (evt->type) {
	case UART_TX_DONE:
		LOG_DBG("UART_TX_DONE");
		if ((evt->data.tx.len == 0) ||
		    (!evt->data.tx.buf)) {
			return;
		}

		if (aborted_buf) {
			buf = CONTAINER_OF(aborted_buf, struct uart_data_t,
					   data[0]);
			aborted_buf = NULL;
			aborted_len = 0;
		} else {
			buf = CONTAINER_OF(evt->data.tx.buf, struct uart_data_t,
					   data[0]);
		}

		k_free(buf);

		buf = k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
		if (!buf) {
			return;
		}

		if (uart_tx(uart, buf->data, buf->len, SYS_FOREVER_MS)) {
			LOG_WRN("Failed to send data over UART");
		}

		break;

	case UART_RX_RDY:
		LOG_DBG("UART_RX_RDY");
		buf = CONTAINER_OF(evt->data.rx.buf, struct uart_data_t, data[0]);
		buf->len += evt->data.rx.len;

		if (disable_req) {
			return;
		}

		if ((evt->data.rx.buf[buf->len - 1] == '\n') ||
		    (evt->data.rx.buf[buf->len - 1] == '\r')) {
			disable_req = true;
			uart_rx_disable(uart);
		}

		break;

	case UART_RX_DISABLED:
		LOG_DBG("UART_RX_DISABLED");
		disable_req = false;

		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
		} else {
			LOG_WRN("Not able to allocate UART receive buffer");
			k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
			return;
		}

		uart_rx_enable(uart, buf->data, sizeof(buf->data),
			       UART_WAIT_FOR_RX);

		break;

	case UART_RX_BUF_REQUEST:
		LOG_DBG("UART_RX_BUF_REQUEST");
		buf = k_malloc(sizeof(*buf));
		if (buf) {
			buf->len = 0;
			uart_rx_buf_rsp(uart, buf->data, sizeof(buf->data));
		} else {
			LOG_WRN("Not able to allocate UART receive buffer");
		}

		break;

	case UART_RX_BUF_RELEASED:
		LOG_DBG("UART_RX_BUF_RELEASED");
		buf = CONTAINER_OF(evt->data.rx_buf.buf, struct uart_data_t,
				   data[0]);

		if (buf->len > 0) {
			k_fifo_put(&fifo_uart_rx_data, buf);
		} else {
			k_free(buf);
		}

		break;

	case UART_TX_ABORTED:
		LOG_DBG("UART_TX_ABORTED");
		if (!aborted_buf) {
			aborted_buf = (uint8_t *)evt->data.tx.buf;
		}

		aborted_len += evt->data.tx.len;
		buf = CONTAINER_OF((void *)aborted_buf, struct uart_data_t,
				   data);

		uart_tx(uart, &buf->data[aborted_len],
			buf->len - aborted_len, SYS_FOREVER_MS);

		break;

	default:
		break;
	}
}

static void uart_work_handler(struct k_work *item)
{
	struct uart_data_t *buf;

	buf = k_malloc(sizeof(*buf));
	if (buf) {
		buf->len = 0;
	} else {
		LOG_WRN("Not able to allocate UART receive buffer");
		k_work_reschedule(&uart_work, UART_WAIT_FOR_BUF_DELAY);
		return;
	}

	uart_rx_enable(uart, buf->data, sizeof(buf->data), UART_WAIT_FOR_RX);
}

static bool uart_test_async_api(const struct device *dev)
{
	const struct uart_driver_api *api =
			(const struct uart_driver_api *)dev->api;

	return (api->callback_set != NULL);
}

static int uart_init(void)
{
	int err;
	int pos;
	struct uart_data_t *rx;
	struct uart_data_t *tx;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

	rx = k_malloc(sizeof(*rx));
	if (rx) {
		rx->len = 0;
	} else {
		return -ENOMEM;
	}

	k_work_init_delayable(&uart_work, uart_work_handler);


	if (IS_ENABLED(CONFIG_UART_ASYNC_ADAPTER) && !uart_test_async_api(uart)) {
		/* Implement API adapter */
		uart_async_adapter_init(async_adapter, uart);
		uart = async_adapter;
	}

	err = uart_callback_set(uart, uart_cb, NULL);
	if (err) {
		k_free(rx);
		LOG_ERR("Cannot initialize UART callback");
		return err;
	}

	if (IS_ENABLED(CONFIG_UART_LINE_CTRL)) {
		LOG_INF("Wait for DTR");
		while (true) {
			uint32_t dtr = 0;

			uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr);
			if (dtr) {
				break;
			}
			/* Give CPU resources to low priority threads. */
			k_sleep(K_MSEC(100));
		}
		LOG_INF("DTR set");
		err = uart_line_ctrl_set(uart, UART_LINE_CTRL_DCD, 1);
		if (err) {
			LOG_WRN("Failed to set DCD, ret code %d", err);
		}
		err = uart_line_ctrl_set(uart, UART_LINE_CTRL_DSR, 1);
		if (err) {
			LOG_WRN("Failed to set DSR, ret code %d", err);
		}
	}

	tx = k_malloc(sizeof(*tx));

	if (tx) {
		pos = snprintf(tx->data, sizeof(tx->data),
			       "Starting Nordic UART service sample\r\n");

		if ((pos < 0) || (pos >= sizeof(tx->data))) {
			k_free(rx);
			k_free(tx);
			LOG_ERR("snprintf returned %d", pos);
			return -ENOMEM;
		}

		tx->len = pos;
	} else {
		k_free(rx);
		return -ENOMEM;
	}

	err = uart_tx(uart, tx->data, tx->len, SYS_FOREVER_MS);
	if (err) {
		k_free(rx);
		k_free(tx);
		LOG_ERR("Cannot display welcome message (err: %d)", err);
		return err;
	}

	err = uart_rx_enable(uart, rx->data, sizeof(rx->data), UART_WAIT_FOR_RX);
	if (err) {
		LOG_ERR("Cannot enable uart reception (err: %d)", err);
		/* Free the rx buffer only because the tx buffer will be handled in the callback */
		k_free(rx);
	}

	return err;
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("Advertising successfully started");
}

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed, err 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", addr);

	current_conn = bt_conn_ref(conn);

	dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	if (auth_conn) {
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
		dk_set_led_off(CON_STATUS_LED);
	}
}

static void recycled_cb(void)
{
	LOG_INF("Connection object available from previous conn. Disconnect is complete!");
	advertising_start();
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security changed: %s level %u", addr, level);
	} else {
		LOG_WRN("Security failed: %s level %u err %d %s", addr, level, err,
			bt_security_err_to_str(err));
	}
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected        = connected,
	.disconnected     = disconnected,
	.recycled         = recycled_cb,
#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};

#if defined(CONFIG_BT_NUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Passkey for %s: %06u", addr, passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	auth_conn = bt_conn_ref(conn);

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Passkey for %s: %06u", addr, passkey);

	if (IS_ENABLED(CONFIG_SOC_SERIES_NRF54H) || IS_ENABLED(CONFIG_SOC_SERIES_NRF54L)) {
		LOG_INF("Press Button 0 to confirm, Button 1 to reject.");
	} else {
		LOG_INF("Press Button 1 to confirm, Button 2 to reject.");
	}
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing cancelled: %s", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Pairing failed conn: %s, reason %d %s", addr, reason,
		bt_security_err_to_str(reason));
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
			  uint16_t len)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN] = {0};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, ARRAY_SIZE(addr));

	LOG_INF("Received data from: %s", addr);

	for (uint16_t pos = 0; pos != len;) {
		struct uart_data_t *tx = k_malloc(sizeof(*tx));

		if (!tx) {
			LOG_WRN("Not able to allocate UART send data buffer");
			return;
		}

		/* Keep the last byte of TX buffer for potential LF char. */
		size_t tx_data_size = sizeof(tx->data) - 1;

		if ((len - pos) > tx_data_size) {
			tx->len = tx_data_size;
		} else {
			tx->len = (len - pos);
		}

		memcpy(tx->data, &data[pos], tx->len);

		pos += tx->len;

		/* Append the LF character when the CR character triggered
		 * transmission from the peer.
		 */
		if ((pos == len) && (data[len - 1] == '\r')) {
			tx->data[tx->len] = '\n';
			tx->len++;
		}

		err = uart_tx(uart, tx->data, tx->len, SYS_FOREVER_MS);
		if (err) {
			k_fifo_put(&fifo_uart_tx_data, tx);
		}
	}
}

/* 2MIC DNS Beamformer over BLE NUS */

#define MIC_SAMPLE_RATE_HZ 16000
#define MIC_WORD_SIZE_BITS 16
#define MIC_NUM_CHANNELS 2
#define MIC_BLOCK_SAMPLE_PAIRS 256
#define MIC_BLOCK_SIZE_BYTES (MIC_BLOCK_SAMPLE_PAIRS * MIC_NUM_CHANNELS * sizeof(int16_t))
#define MIC_BLOCK_COUNT 4

K_MEM_SLAB_DEFINE(mic_rx_mem_slab, MIC_BLOCK_SIZE_BYTES, MIC_BLOCK_COUNT, 4);

#define TDM_NODE DT_NODELABEL(tdm)
static const struct device *i2s_dev = DEVICE_DT_GET(TDM_NODE);

static struct i2s_config i2s_cfg = {
	.word_size = MIC_WORD_SIZE_BITS,
	.channels = MIC_NUM_CHANNELS,
	.format = I2S_FMT_DATA_FORMAT_I2S,
	.options = I2S_OPT_BIT_CLK_CONTOLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
	.frame_clk_freq = MIC_SAMPLE_RATE_HZ,
	.mem_slab =  &mic_rx_mem_slab,
	.block_size = MIC_BLOCK_SIZE_BYTES,
	.timeout = 2000,
};

#define AUDIO_CHUNK_SAMPLES 512

struct audio_chunk {
	sys_snote_t node;
	uint16_t count;
	int16_t samples[AUDIO_CHUNK_SAMPLES];
};

#define MAX_RECORDING_SAMPLES (MIC_SAMPLE_RATE_HZ * 8)

static sys_slist_t audio_chunks;
static struct audio_chunk *audio_tail;
static uint32_t total_samples_captured;

enum recording_state {
	RECORDING_IDLE,
	RECORDING_ACTIVE,
};

static enum recording_state rec_state = RECORDING_IDLE;

static atomic_t stop_requested = ATOMIC_INIT(0);
static atomic_t data_pending_send = ATOMIC_INIT(0);
static atomic_t notifications_enabled = ATOMIC_INIT(0);

static K_SEM_DEFINE(audio_tx_sem, 0, 1);

static structu k_thread capture_thread_data;
K_THREAD_STACK_DEFINE(capture_thread_stack, 2048);

#define CAPTURE_THREAD_PRIORITY 6

static void audio_buffer_append(int16_t sample) {
	if (!audio_tail || audio_tail->count >= AUDIO_CHUNK_SAMPLES) {
		struct audio_chunk *chunk = k_malloc(sizeof(struct audio_chunk));

		if(!chunk) {
			LOG_ERR("Out of memory growing audio buffer - dropping sample");
			return;
		}

		chunk->count = 0;
		sys_slist_append(&audio_chunks, &chunk->node);
		audio_tail = chunk;
	}

	audio_tail->samples[audio_tail->count++] = sample;
	total_samples_captured++;
}

static void audio_buffer_reset(void) {
	struct audio_chunk *chunk, *tmp;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&audio_chunks, chunk, tmp, node) {
		k_free(chunk);
	}
	
	sys_slist_init(&audio_chunks);
	audio_tail = NULL;
	total_samples_captured = 0;
}

static void capture_thread_fn(void *p1, void *p2, void *p3) {
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int err = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);

	if(err) {
		LOG_ERR("Failed to start I2S RX (err %d)", err);
		rec_state = RECORDING_IDLE;
		return;
	}

	uint32_t samples_since_log = 0;

	while(!atomic_get(&stop_requested)) {
		void *mem_block;
		size_t block_size;

		err = i2s_read(i2s_dev, &mem_block, &block_size);
		if (err) {
			if(err == -EAGAIN) {
				LOG_WRN("I2S read timed out, retrying");
				continue;
			}
			break;
		}

		int16_t *stereo = (int16_t *)mem_block;
		size_t num_pairs = block_size / (MIC_NUM_CHANNELS * sizeof(int16_t));

		for(size_t i = 0; i < num_pairs; i++) {
			int16_t left = stereo[2*i];
			int16_t right = stereo[2*i + i];
			
			int32_t sum = (int32_t)left + (int32_t)right;
			int16_t mono = (int16_t)(sum/2);

			audio_buffer_append(mono);
			samples_since_log++;

			if(total_sampels_captured >= MAX_RECORDING_SAMPLES) {
				LOG_WRN("Max recording length reached (%u samples)", "auto-stopping", (unsigned int)MAX_RECORDING_SAMPLES);
				atomic_set(&stop_requested, 1);
				break;
			}
		}

		k_mem_slab_free(&mic_rx_mem_slab, mem_block);

		if(samples_since_log >= MIC_SAMPLE_RATE_HZ) {
			LOG_INF("Recording in progress: %u samples captured (%u ms)", total_samples_captured, (total_samples_captured * 1000) / MIC_SAMPLE_RATE_HZ);
			samples_since_log = 0;
		}
	}

	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);

	LOG_INF("Recording stopped, samples captured: %u", total_samples_captured);

	rec_state = RECORDING_IDLE;
	atomic_set(&data_pending_send,1);

	if(!atomic_get(&notifications_enabled)) {
		LOG_WRN("No BLE central subscribed to notifications - buffering %u samples, will send automatically once notifications are enabled", total_samples_captured);
	}

	k_sem_give(&audio_tx_sem);
}

static void handle_start_recording(void) {
	if (rec_state == RECORDING_ACTIVE) {
		LOG_WRN("Button 0 pressed while already recording - ignoring");
		return;
	}

	if (atomic_get(&data_pending_send)) {
		LOG_WRN("Button 0 pressed but the previous recording hasn't finished transmitting yet - ignoring");
		return;
	}

	if(!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S/TDM device not ready - cannot start recording");
		return;
	}

	audio_buffer_reset();
	atomic_set(&stop_requested,0);
	rec_state = RECORDING_ACTIVE;

	k_tid_t tid = k_thread_create(&capture_thread_data, capture_thread_stack, K_THREAD_STACK_SIZEOF(capture_thread_stack), capture_thread_fn, NULL, NULL, NULL, CAPTURE_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(tid, "audio_capture");

	LOG_INF("Recording started");
}

static void handle_stop_recording(void) {
	if(rec_state != RECORDING_ACTIVE) {
		LOG_WRN("Button 1 pressed but no recording is in progress - ignoring");
		return;
	}

	atomic_set(&stop_requested, 1);
	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
}

static void send_beamformed_audio(void) {
	if(!current_conn) {
		LOG_WRN("No active BLE connection - keeping %u samples buffered", total_samples_captured);
		return;
	}

	uint16_t att_mtu = bt_gatt_get_mtu(current_conn);
	uint16_t max_att_payload = (att_mtu > 3) ? (att_mtu - 3) : 20;
	
	uint16_t chunk_samples = MAX(1, max_att_payload / (int)sizeof(int16_t));
	uint16_t chunk_bytes = chunk_samples * sizeof(int16_t);

	uint32_t total_chunks = DIV_ROUND_UP(total_samples_captured, chunk_samples);
	uint32_t chunk_num = 0;

	LOG_INF("Starting BLE transmission: %u samples, ATT MTU %u, %u bytes/chunk, %u chunks total", total_samples_captured, att_mtu, chunk_bytes, total_chunks);

	struct audio_chunk *storage_chunk;

	SYS_SLIST_FOR_EACH_CONTAINER(&audio_chunks, storage_chunk, node) {
		uint16_t offset = 0;

		while(offset < storage_chunk->count) {
			uint16_t remaining = storage_chunk->count - offset;
			uint16_t send_samples = MIN(chunk_samples, remaining);
			uint16_t send_bytes = send_samples * sizeof(int16_t);

			int err = bt_nus_send(NULL, (const uint8_t *)&storage_chunk -> samples[offset], send_bytes);
			chunk_num++;

			if(err) {
				LOG_WRN("bt_nus_send failed (err %d) on chunk %u/%u - aborting, %u samples remain buffered for retry", err, chunk_num, total_chunks, total_samples_captured);
				return;
			}

			LOG_INF("Sending chunk %u/%u", chunk_num, total_chunks);

			offset += send_samples;
			k_sleep(K_MSEC(20));
		}
	}

	LOG_INF("Bluetooth transfer complete");

	audio_buffer_reset();
	atomic_clear(&data_pending_send);
}

static void ble_tx_thread(void *p1, void *p2, void *p3) {
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for(;;) { 
		k_sem_take(&audio_tx_sem, K_FOREVER);

		if(!atomic_get(&data_pending_send)) {
			continue;
		}

		if(!atomic_get(&notifications_enabled)) {
			continue;
		}

		send_beamformed_audio();
	}
}

#define TX_THREAD_STACK_SIZE 2048
#define TX_THREAD_CAPACITY 7

K_THREAD_DEFINE(ble_tx_thread_id, TX_THREAD_STACK_SIZE, ble_tx_thread, NULL, NULL, NULL, TX_THREAD_PRIORITY, 0, 0);

static void nus_send_enabled_cb(enum bt_nus_send_status status) {
	bool enabled = (status == BT_NUS_SEND_STATUS_ENABLED);

	atomic_set(&notifications_enabled, enabled ? 1:0);

	if(enabled) {
		LOG_INF("BLE notifications enabled by central");
		k_sem_give(&audio_tx_sem);
	} else {
		LOG_INF("BLE notifications disabled");
	}
}

static struct bt_nus_cb nus_cb = {
	.received = bt_receive_cb,
	.send_enabled = nus_send_enabled_cb,
};

void error(void) {
	dk_set_leds_stats(DK_ALL_LEDS_MSK, DK_NO_LEDS_MSK);
	while (true) {
		k_sleep(K_MSEC(1000));
	}
}

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
static void num_comp_reply(bool accept) {
	if(accept) {
		bt_conn_auth_passkey_confirm(auth_conn);
		LOG_INF("Numeric Match, conn %p", (void *)auth_conn);
	} else {
		bt_conn_auth_cancel(auth_conn);
		LOG_INF("Numeric Reject, conn %p", (void *)auth_conn);
	}

	bt_conn_unref(auth_conn);
	auth_conn = NULL;
}
#endif

void button_changed(uint32_t button_state, uint32_t has_changed) {
	uint32_t buttons = button_state & has_changed;

#ifdef CONFIG_BT_NUS_SECURITY_ENABLED
	if (auth_conn) {
		if(buttons & KEY_PASSWORD_ACCEPT) {
			num_comp_reply(true);
		}

		if(buttons & KEY_PASSKEY_REJECT) {
			num_comp_reply(false);
		}
	}
#endif

	if(buttons & RECORD_START_BTN_MSK) {
		handle_start_recording();
	}

	if(buttons & RECORD_STOP_BTN_MSK) {
		handle_stop_recording();
	}
}

static void configure_gpio(void) {
	int err;

	err = dk_buttons_init(button_changed);
	if(err) {
		LOG_ERR("Cannot init buttons (err: %d)", err);
	}

	err = dk_leds_init();
	if(err) {
		LOG_ERR("Cannot init LEDs (err: %d)", err);
	}
}

int main(void)
{
	int blink_status = 0;
	int err = 0;

	configure_gpio();

	err = uart_init();
	if (err) {
		error();
	}

	if (IS_ENABLED(CONFIG_BT_NUS_SECURITY_ENABLED)) {
		err = bt_conn_auth_cb_register(&conn_auth_callbacks);
		if (err) {
			LOG_ERR("Failed to register authorization callbacks. (err: %d)", err);
			return 0;
		}

		err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
		if (err) {
			LOG_ERR("Failed to register authorization info callbacks. (err: %d)", err);
			return 0;
		}
	}

	err = bt_enable(NULL);
	if (err) {
		error();
	}

	LOG_INF("Bluetooth initialized");

	k_sem_give(&ble_init_ok);

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("Failed to initialize UART service (err: %d)", err);
		return 0;
	}

	/* I2S MICROPHONE INTERFACE */
	if (!device_is_read(i2s_dev)) {
		LOG_ERR("I2S/TDM device not ready - beamformer recording will be unavailable");
	} else {
		err = i2s_configure(i2s_dev,I2S_DIR_RX, &i2s_cfg);
		if(err) {
			LOG_ERR("Failed to configure I2S RX (err %d)", err);
		} else {
			LOG_INF("I2S/TDM microphone interface configured (%u Hz, %u-bit, %u ch)", i2s_cfg.frame_clk_freq, i2s_cfg.word_size, i2s_cfg.channels);
		}
	}

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

	// added code to test chip -> BLE -> iOS app pipeline
	static const char test_msg[] = "Hello from chip";


	for (;;) {
		dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));

		bt_nus_send(NULL, test_msg, sizeof(test_msg) - 1);
	}
}

void ble_write_thread(void)
{
	/* Don't go any further until BLE is initialized */
	k_sem_take(&ble_init_ok, K_FOREVER);
	struct uart_data_t nus_data = {
		.len = 0,
	};

	for (;;) {
		/* Wait indefinitely for data to be sent over bluetooth */
		struct uart_data_t *buf = k_fifo_get(&fifo_uart_rx_data, K_FOREVER);

		int plen = MIN(sizeof(nus_data.data) - nus_data.len, buf->len);
		int loc = 0;

		while (plen > 0) {
			memcpy(&nus_data.data[nus_data.len], &buf->data[loc], plen);
			nus_data.len += plen;
			loc += plen;

			if (nus_data.len >= sizeof(nus_data.data) ||
			   (nus_data.data[nus_data.len - 1] == '\n') ||
			   (nus_data.data[nus_data.len - 1] == '\r')) {
				if (bt_nus_send(NULL, nus_data.data, nus_data.len)) {
					LOG_WRN("Failed to send data over BLE connection");
				}
				nus_data.len = 0;
			}
			plen = MIN(sizeof(nus_data.data), buf->len - loc);
		}
		k_free(buf);
	}
}

K_THREAD_DEFINE(ble_write_thread_id, STACKSIZE, ble_write_thread, NULL, NULL, NULL, PRIORITY, 0, 0);