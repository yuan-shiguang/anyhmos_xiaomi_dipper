/* SPDX-License-Identifier: MIT */
#ifndef DIPPER_PAL_CLIENT_H
#define DIPPER_PAL_CLIENT_H
#include "protocol.h"
int dipper_pal_connect(void);
int dipper_pal_request(int fd, uint32_t op, uint32_t arg0, uint32_t arg1,
    const void *pcm, uint32_t bytes, struct dipper_pal_reply *reply);
int dipper_pal_capture_read(int fd, void *pcm, uint32_t capacity,
    struct dipper_pal_reply *reply);
#endif
