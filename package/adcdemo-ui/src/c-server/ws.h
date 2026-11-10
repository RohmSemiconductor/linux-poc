/* SPDX-License-Identifier: GPL-2.0 */
#ifndef WS_H
#define WS_H

#include <stddef.h>
#include <stdint.h>

#define WS_OP_TEXT	0x1
#define WS_OP_BINARY	0x2
#define WS_OP_CLOSE	0x8
#define WS_OP_PING	0x9
#define WS_OP_PONG	0xa

/* Longest frame header written by ws_frame_header() */
#define WS_MAX_HDR	10

/* Compute Sec-WebSocket-Accept for @key. @out gets 28 chars + NUL */
void ws_accept_key(const char *key, char out[29]);

/*
 * Write the header of an unmasked, unfragmented server frame into @hdr.
 * Returns the header length (2, 4 or 10 bytes).
 */
size_t ws_frame_header(uint8_t *hdr, uint8_t opcode, size_t len);

#endif
