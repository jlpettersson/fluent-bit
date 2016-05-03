/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2016 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <unistd.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_engine.h>
#include <fluent-bit/flb_network.h>

#include "fw.h"
#include "fw_prot.h"
#include "fw_conn.h"

/* Callback invoked every time an event is triggered for a connection */
int fw_conn_event(void *data)
{
    int ret;
    int bytes;
    int available;
    int size;
    char *tmp;
    struct mk_event *event;
    struct fw_conn *conn = data;

    event = &conn->event;
    if (event->mask & MK_EVENT_READ) {
        available = (conn->buf_size - conn->buf_len);
        if (available < 1) {
            if (conn->buf_size + FLB_IN_FW_CHUNK > conn->ctx->buffer_size) {
                flb_trace("[in_fw] fd=%i incoming data exceed limit (%i KB)",
                          event->fd, conn->buf_size);
                fw_conn_del(conn);
                return -1;
            }
            size = conn->buf_size + FLB_IN_FW_CHUNK;
            tmp = realloc(conn->buf, size);
            if (!tmp) {
                perror("realloc");
                return -1;
            }
            flb_trace("[in_fw] fd=%i buffer realloc %i -> %i",
                      event->fd, conn->buf_size, size);

            conn->buf = tmp;
            conn->buf_size = size;
            available = (conn->buf_size - conn->buf_len);
        }

        bytes = read(conn->fd,
                     conn->buf + conn->buf_len, available);
        if (bytes > 0) {
            conn->buf_len += bytes;
            flb_trace("[in_fw] read()=%i", bytes);

            ret = fw_prot_process(conn);
            if (ret == -1) {
                return -1;
            }
            return bytes;
        }
        else {
            flb_trace("[in_fw] fd=%i closed connection", event->fd);
            fw_conn_del(conn);
            return -1;
        }
    }
    else if (event->mask & MK_EVENT_CLOSE) {
        flb_trace("[in_fw] fd=%i hangup", event->fd);
    }
    return 0;
}

/* Create a new mqtt request instance */
struct fw_conn *fw_conn_add(int fd, struct flb_in_fw_config *ctx)
{
    int ret;
    struct fw_conn *conn;
    struct mk_event *event;

    conn = malloc(sizeof(struct fw_conn));
    if (!conn) {
        return NULL;
    }

    /* Set data for the event-loop */
    event = &conn->event;
    event->fd           = fd;
    event->type         = FLB_ENGINE_EV_CUSTOM;
    event->mask         = MK_EVENT_EMPTY;
    event->handler      = fw_conn_event;
    event->status       = MK_EVENT_NONE;

    /* Connection info */
    conn->fd      = fd;
    conn->ctx     = ctx;
    conn->buf_len = 0;
    conn->buf_off = 0;
    conn->status  = FW_NEW;

    conn->buf = malloc(FLB_IN_FW_CHUNK);
    if (!conn->buf) {
        perror("malloc");
        close(fd);
        flb_error("[in_fw] could not allocate new connection");
        free(conn);
        return NULL;
    }
    conn->buf_size = FLB_IN_FW_CHUNK;

    /* Register instance into the event loop */
    ret = mk_event_add(ctx->evl, fd, FLB_ENGINE_EV_CUSTOM, MK_EVENT_READ, conn);
    if (ret == -1) {
        flb_error("[in_fw] could not register new connection");
        close(fd);
        free(conn->buf);
        free(conn);
        return NULL;
    }

    return conn;
}

int fw_conn_del(struct fw_conn *conn)
{
    /* Unregister the file descriptior from the event-loop */
    mk_event_del(conn->ctx->evl, &conn->event);

    /* Release resources */
    close(conn->fd);
    free(conn->buf);
    free(conn->tag);
    free(conn);

    return 0;
}