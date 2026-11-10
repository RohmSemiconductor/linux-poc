// SPDX-License-Identifier: GPL-2.0
/* Minimal WebSocket (RFC 6455) helpers: handshake key and frame header. */
#include <string.h>

#include "ws.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

struct sha1 {
	uint32_t h[5];
	uint8_t buf[64];
	uint64_t total;
	size_t fill;
};

static uint32_t rol(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

static void sha1_block(struct sha1 *s, const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e, f, k, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 |
		       p[4 * i + 2] << 8 | p[4 * i + 3];
	for (; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		t = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = t;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
}

static void sha1_update(struct sha1 *s, const uint8_t *p, size_t len)
{
	s->total += len;
	while (len) {
		size_t n = 64 - s->fill;

		if (n > len)
			n = len;
		memcpy(s->buf + s->fill, p, n);
		s->fill += n;
		p += n;
		len -= n;
		if (s->fill == 64) {
			sha1_block(s, s->buf);
			s->fill = 0;
		}
	}
}

static void sha1_final(struct sha1 *s, uint8_t out[20])
{
	uint64_t bits = s->total * 8;
	uint8_t pad = 0x80;
	uint8_t zero = 0;
	uint8_t len[8];
	int i;

	sha1_update(s, &pad, 1);
	while (s->fill != 56)
		sha1_update(s, &zero, 1);
	for (i = 0; i < 8; i++)
		len[i] = bits >> (56 - 8 * i);
	sha1_update(s, len, 8);
	for (i = 0; i < 20; i++)
		out[i] = s->h[i / 4] >> (24 - 8 * (i % 4));
}

static void base64_20(const uint8_t *in, char *out)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i;

	for (i = 0; i < 18; i += 3) {
		uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];

		*out++ = tbl[v >> 18];
		*out++ = tbl[(v >> 12) & 63];
		*out++ = tbl[(v >> 6) & 63];
		*out++ = tbl[v & 63];
	}
	/* 20 bytes: two bytes remain, giving 3 chars and one '=' */
	*out++ = tbl[in[18] >> 2];
	*out++ = tbl[((in[18] & 3) << 4) | (in[19] >> 4)];
	*out++ = tbl[(in[19] & 15) << 2];
	*out++ = '=';
	*out = '\0';
}

void ws_accept_key(const char *key, char out[29])
{
	struct sha1 s = {
		.h = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
		       0xc3d2e1f0 },
	};
	uint8_t digest[20];

	sha1_update(&s, (const uint8_t *)key, strlen(key));
	sha1_update(&s, (const uint8_t *)WS_GUID, strlen(WS_GUID));
	sha1_final(&s, digest);
	base64_20(digest, out);
}

size_t ws_frame_header(uint8_t *hdr, uint8_t opcode, size_t len)
{
	int i;

	hdr[0] = 0x80 | opcode;
	if (len < 126) {
		hdr[1] = len;
		return 2;
	}
	if (len < 65536) {
		hdr[1] = 126;
		hdr[2] = len >> 8;
		hdr[3] = len;
		return 4;
	}
	hdr[1] = 127;
	for (i = 0; i < 8; i++)
		hdr[2 + i] = (uint64_t)len >> (56 - 8 * i);
	return 10;
}
