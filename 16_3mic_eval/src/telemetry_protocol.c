#include <string.h>

#include "telemetry_protocol.h"

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout. */
uint16_t tp_crc16(uint16_t crc, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static inline void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

size_t tp_encode(uint8_t *out, size_t out_cap, const struct tp_header *h,
		 const void *payload, size_t payload_len)
{
	size_t total = TP_HEADER_LEN + payload_len + TP_CRC_LEN;

	if (payload_len > TP_MAX_PAYLOAD || out_cap < total) {
		return 0;
	}

	out[0] = TP_MAGIC0;
	out[1] = TP_MAGIC1;
	out[2] = TP_VERSION;
	out[3] = h->type;
	put16(out + 4, h->flags);
	put16(out + 6, h->run_id);
	put16(out + 8, h->stream_id);
	put32(out + 10, h->seq);
	put32(out + 14, h->timestamp);
	put16(out + 18, (uint16_t)payload_len);
	if (payload_len) {
		memcpy(out + TP_HEADER_LEN, payload, payload_len);
	}

	uint16_t crc = tp_crc16(0xFFFFU, out + 2, TP_HEADER_LEN - 2U + payload_len);

	put16(out + TP_HEADER_LEN + payload_len, crc);
	return total;
}

void tp_parser_init(struct tp_parser *p)
{
	memset(p, 0, sizeof(*p));
}

static void parser_reset(struct tp_parser *p)
{
	p->len = 0;
	p->expected = 0;
	p->in_packet = false;
}

tp_parse_result_t tp_parser_feed(struct tp_parser *p, uint8_t byte)
{
	if (!p->in_packet) {
		if (p->len == 0 && byte == TP_MAGIC0) {
			p->buf[p->len++] = byte;
			return TP_PARSE_NONE;
		}
		if (p->len == 1 && p->buf[0] == TP_MAGIC0) {
			if (byte == TP_MAGIC1) {
				p->buf[p->len++] = byte;
				p->in_packet = true;
				return TP_PARSE_NONE;
			}
			/* Lone 0xA5 was text after all; fall through as text. */
			p->len = 0;
			if (byte == '\n' || byte == '\r') {
				return TP_PARSE_NONE;
			}
		}

		/* Text line accumulation. */
		if (byte == '\n' || byte == '\r') {
			if (p->len == 0) {
				return TP_PARSE_NONE;
			}
			p->buf[p->len] = '\0';
			/* Caller reads buf/len; reset happens on next feed via len=0 below. */
			size_t n = p->len;

			p->len = 0;
			p->expected = n;   /* stash line length for the caller */
			return TP_PARSE_TEXT_LINE;
		}
		if (p->len < TP_MAX_PACKET - 1U) {
			p->buf[p->len++] = byte;
		} else {
			p->len = 0;   /* overlong line, discard */
		}
		return TP_PARSE_NONE;
	}

	/* Inside a packet. */
	if (p->len < TP_MAX_PACKET) {
		p->buf[p->len++] = byte;
	}

	if (p->len == TP_HEADER_LEN) {
		uint16_t plen = get16(p->buf + 18);

		if (p->buf[2] != TP_VERSION || plen > TP_MAX_PAYLOAD) {
			p->resyncs++;
			parser_reset(p);
			return TP_PARSE_NONE;
		}
		p->expected = TP_HEADER_LEN + plen + TP_CRC_LEN;
		return TP_PARSE_NONE;
	}

	if (p->expected && p->len == p->expected) {
		uint16_t want = get16(p->buf + p->expected - 2U);
		uint16_t got = tp_crc16(0xFFFFU, p->buf + 2, p->expected - 2U - TP_CRC_LEN);

		if (want != got) {
			p->bad_crc++;
			parser_reset(p);
			return TP_PARSE_BAD_CRC;
		}
		p->hdr.type = p->buf[3];
		p->hdr.flags = get16(p->buf + 4);
		p->hdr.run_id = get16(p->buf + 6);
		p->hdr.stream_id = get16(p->buf + 8);
		p->hdr.seq = get32(p->buf + 10);
		p->hdr.timestamp = get32(p->buf + 14);
		p->hdr.payload_len = get16(p->buf + 18);
		/* Leave buf intact for the caller; next feed starts fresh. */
		p->len = 0;
		p->in_packet = false;
		return TP_PARSE_PACKET;
	}

	return TP_PARSE_NONE;
}
