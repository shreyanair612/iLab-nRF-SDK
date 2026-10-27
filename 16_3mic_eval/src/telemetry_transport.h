/*
 * Telemetry transport over the DK's USB high-speed port (CDC ACM).
 *
 * Device -> host: a lock-protected transmit ring, drained by the CDC ACM
 * class's interrupt-driven UART API whenever the host has room. Audio capture
 * never waits on the link; when the ring is full a packet is dropped and
 * counted, and the host sees the hole in the audio sample index.
 *
 * Host -> device: bytes are read in the UART callback into a byte ring and
 * parsed by a small thread. Framed packets and plain text lines are both
 * accepted, so a terminal can drive the device without any tooling.
 *
 * Text vs binary mode: the device boots in text mode and prints readable
 * lines. The host tool sends "binary_mode on" and every outgoing message is
 * then framed (TP_TEXT for human lines, TP_AUDIO_CHUNK for audio).
 *
 * Zephyr logging stays on uart20 (the J-Link virtual COM port), separate
 * from this link, so log text can never tear a binary packet apart and the
 * debug console keeps working even when the USB cable is unplugged.
 */
#ifndef TELEMETRY_TRANSPORT_H_
#define TELEMETRY_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetry_protocol.h"

struct tt_stats {
	uint32_t bytes_sent;
	uint32_t packets_sent;
	uint32_t packets_dropped;    /* ring full */
	uint32_t audio_dropped;      /* subset of packets_dropped that were audio */
	uint32_t ring_high_water;
	uint32_t ring_size;
	uint32_t rx_bytes;
	uint32_t rx_packets;
	uint32_t rx_bad_crc;
	uint32_t rx_resyncs;
	uint32_t rx_overflow;
};

typedef void (*tt_rx_line_cb_t)(const char *line);
typedef void (*tt_rx_packet_cb_t)(const struct tp_header *h, const uint8_t *payload);

int tt_init(void);
void tt_set_rx_callbacks(tt_rx_line_cb_t line_cb, tt_rx_packet_cb_t packet_cb);

void tt_set_binary_mode(bool on);
bool tt_binary_mode(void);

/* True while a host has the CDC ACM port open (DTR asserted). */
bool tt_host_connected(void);

/* Framed send. Returns 0, or -ENOSPC if the ring cannot take the whole packet. */
int tt_send(const struct tp_header *h, const void *payload, size_t len);

/* Text-type packet (TP_LOG, TP_ACK, TP_TEXT, TP_ERROR). In text mode the
 * line is written raw. printf-style; the line must fit in 240 bytes. */
int tt_send_text(uint8_t type, const char *fmt, ...);

/* JSON-type packet. printf-style; payload must fit in 960 bytes. In text mode
 * the payload is written as "<TYPE> {json}". */
int tt_send_json(uint8_t type, uint16_t run_id, uint16_t stream_id, uint32_t seq,
		 uint32_t timestamp, uint16_t flags, const char *fmt, ...);

/* Audio chunk. Silently discarded in text mode. */
int tt_send_audio(uint16_t run_id, uint16_t stream_id, uint32_t seq, uint32_t sample_index,
		  uint16_t flags, const int16_t *pcm, size_t samples);

void tt_get_stats(struct tt_stats *out);
void tt_reset_stats(void);

/* Bytes currently queued, for queue-watermark telemetry. */
uint32_t tt_ring_used(void);

#endif /* TELEMETRY_TRANSPORT_H_ */
