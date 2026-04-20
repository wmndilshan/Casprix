#ifndef CASPRIX_WEBSOCKET_FRAMING_H
#define CASPRIX_WEBSOCKET_FRAMING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CPX_WS_OP_CONTINUATION = 0x0,
    CPX_WS_OP_TEXT         = 0x1,
    CPX_WS_OP_BINARY       = 0x2,
    CPX_WS_OP_CLOSE        = 0x8,
    CPX_WS_OP_PING         = 0x9,
    CPX_WS_OP_PONG         = 0xA,
} CpxWSOpcode;

typedef struct {
    bool        fin;
    CpxWSOpcode opcode;
    bool        masked;
    uint64_t    payload_len;
    uint8_t     mask_key[4];
    const uint8_t* payload; /* points into input buffer after decode */
} CpxWSFrame;

/* Client→server frames must be masked (RFC 6455). Set masked=1 and random mask_key. */
int cpx_ws_encode_frame(const CpxWSFrame* frame, uint8_t* out_buf, size_t out_size, size_t* out_written);

/* Returns 0 on success, -1 incomplete, -2 error. *consumed = bytes used from in_buf. */
int cpx_ws_decode_frame(const uint8_t* in_buf, size_t in_size, CpxWSFrame* out_frame, size_t* consumed);

#ifdef __cplusplus
}
#endif
#endif
