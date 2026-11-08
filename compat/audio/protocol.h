/* SPDX-License-Identifier: MIT */
#ifndef DIPPER_PAL_PROTOCOL_H
#define DIPPER_PAL_PROTOCOL_H
#include <stdint.h>
#define DIPPER_PAL_MAGIC 0x53415031u
#define DIPPER_PAL_SOCKET "dipper.pal.pcm.v1"
#define DIPPER_PAL_MAX_DATA 61440u
enum dipper_pal_op {
    DIPPER_PAL_OPEN=1, DIPPER_PAL_START, DIPPER_PAL_WRITE, DIPPER_PAL_VOLUME,
    DIPPER_PAL_STOP, DIPPER_PAL_CLOSE, DIPPER_PAL_PAUSE, DIPPER_PAL_RESUME,
    DIPPER_PAL_STATUS,
    DIPPER_PAL_CAPTURE_OPEN, DIPPER_PAL_CAPTURE_READ, DIPPER_PAL_CAPTURE_START,
    DIPPER_PAL_CAPTURE_STOP, DIPPER_PAL_CAPTURE_CLOSE, DIPPER_PAL_CAPTURE_MUTE,
    DIPPER_PAL_CAPTURE_GET_MUTE, DIPPER_PAL_CAPTURE_STATUS,
    DIPPER_PAL_MIC_MUTE, DIPPER_PAL_MIC_GET_MUTE
};
#define DIPPER_PAL_CAPTURE_CHUNK 960u
struct dipper_pal_request {
    uint32_t magic, op, bytes, sequence, arg0, arg1;
};
struct dipper_pal_reply {
    uint32_t magic, sequence;
    int32_t rc;
    uint32_t bytes;
    uint64_t written_frames;
};
_Static_assert(sizeof(struct dipper_pal_request)==24, "fixed wire header");
_Static_assert(sizeof(struct dipper_pal_reply)==24, "fixed wire reply");
#endif
