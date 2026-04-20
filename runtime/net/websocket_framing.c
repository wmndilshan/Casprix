#include "websocket_framing.h"
#include <string.h>

static int write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
    return 2;
}

static int write_u64_be(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[7 - i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
    return 8;
}

int cpx_ws_encode_frame(const CpxWSFrame* frame, uint8_t* out_buf, size_t out_size, size_t* out_written) {
    if (!frame || !out_buf || !out_written) return -1;
    uint64_t plen = frame->payload_len;
    size_t hlen = 2;
    if (plen <= 125) {
    } else if (plen <= 65535) {
        hlen += 2;
    } else {
        hlen += 8;
    }
    if (frame->masked) hlen += 4;
    size_t need = hlen + (size_t)plen;
    if (need > out_size) return -2;

    size_t pos = 0;
    out_buf[pos++] = (uint8_t)((frame->fin ? 0x80 : 0) | ((int)frame->opcode & 0x0F));

    if (plen <= 125) {
        out_buf[pos++] = (uint8_t)((frame->masked ? 0x80 : 0) | (uint8_t)plen);
    } else if (plen <= 65535) {
        out_buf[pos++] = (uint8_t)((frame->masked ? 0x80 : 0) | 126);
        pos += (size_t)write_u16_be(out_buf + pos, (uint16_t)plen);
    } else {
        out_buf[pos++] = (uint8_t)((frame->masked ? 0x80 : 0) | 127);
        pos += (size_t)write_u64_be(out_buf + pos, plen);
    }

    if (frame->masked) {
        memcpy(out_buf + pos, frame->mask_key, 4);
        pos += 4;
        const uint8_t* pay = frame->payload;
        for (uint64_t i = 0; i < plen; i++)
            out_buf[pos + i] = (uint8_t)(pay[i] ^ frame->mask_key[i % 4]);
        pos += (size_t)plen;
    } else {
        if (frame->payload && plen)
            memcpy(out_buf + pos, frame->payload, (size_t)plen);
        pos += (size_t)plen;
    }

    *out_written = pos;
    return 0;
}

int cpx_ws_decode_frame(const uint8_t* in_buf, size_t in_size, CpxWSFrame* out_frame, size_t* consumed) {
    if (!in_buf || !out_frame || !consumed) return -2;
    memset(out_frame, 0, sizeof(*out_frame));
    if (in_size < 2) return -1;

    out_frame->fin    = (in_buf[0] & 0x80) != 0;
    out_frame->opcode = (CpxWSOpcode)(in_buf[0] & 0x0F);

    int masked = (in_buf[1] & 0x80) != 0;
    uint64_t len = in_buf[1] & 0x7F;
    size_t pos = 2;

    if (len == 126) {
        if (in_size < pos + 2) return -1;
        len = ((uint64_t)in_buf[pos] << 8) | in_buf[pos + 1];
        pos += 2;
    } else if (len == 127) {
        if (in_size < pos + 8) return -1;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | in_buf[pos + i];
        pos += 8;
    }

    out_frame->masked     = masked ? true : false;
    out_frame->payload_len = len;

    if (masked) {
        if (in_size < pos + 4) return -1;
        memcpy(out_frame->mask_key, in_buf + pos, 4);
        pos += 4;
    }

    if (in_size < pos + len) return -1;

    out_frame->payload = in_buf + pos;
    /* Unmask in-place for caller convenience: copy not done; reader must XOR */
    *consumed = pos + (size_t)len;
    return 0;
}
