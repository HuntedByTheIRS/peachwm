/*
 * See LICENSE file for copyright and license details.
 * PeachWM IPC Socket - swaymsg-compatible Unix domain socket IPC
 *
 * This header declares the public API for the swaymsg-compatible
 * JSON-based IPC over a Unix domain socket.
 *
 * Protocol (sway/i3-compatible):
 *   Header:  "i3-ipc" magic (6 bytes)
 *            uint32_t payload_length (little-endian)
 *            uint32_t message_type   (little-endian)
 *   Payload: JSON string (UTF-8)
 *
 * Message types:
 *   0 = COMMAND      1 = GET_WORKSPACES   2 = SUBSCRIBE
 *   3 = GET_OUTPUTS  4 = GET_TREE         5 = GET_VERSION
 *
 * Events (type has 0x80000000 bit set):
 *   0x80000000 = workspace   0x80000001 = output
 *   0x80000002 = mode        0x80000003 = window
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

#define IPC_MAGIC        "i3-ipc"
#define IPC_MAGIC_LEN    6
#define IPC_HEADER_SIZE  14  /* magic(6) + len(4) + type(4) */

enum ipc_message_type {
	IPC_COMMAND        = 0,
	IPC_GET_WORKSPACES = 1,
	IPC_SUBSCRIBE      = 2,
	IPC_GET_OUTPUTS    = 3,
	IPC_GET_TREE       = 4,
	IPC_GET_VERSION    = 5,
};

/* Event types use the high bit (0x80000000) which exceeds INT_MAX.
 * Use uint32_t constants instead of enum members to stay C11-clean. */
#define IPC_EVENT_WORKSPACE  ((uint32_t)0x80000000u)
#define IPC_EVENT_OUTPUT     ((uint32_t)0x80000001u)
#define IPC_EVENT_MODE       ((uint32_t)0x80000002u)
#define IPC_EVENT_WINDOW     ((uint32_t)0x80000003u)

enum ipc_sub_flag {
	IPC_SUB_WORKSPACE = 1 << 0,
	IPC_SUB_WINDOW    = 1 << 1,
	IPC_SUB_OUTPUT    = 1 << 2,
	IPC_SUB_MODE      = 1 << 3,
};

/* ------------------------------------------------------------------ */
/* Per-client state                                                    */
/* ------------------------------------------------------------------ */

struct ipc_client {
	struct wl_list link;
	int fd;
	uint32_t subscribed;
	struct wl_event_source *src;
	/* partial-read state */
	uint8_t hdr_buf[IPC_HEADER_SIZE];
	size_t hdr_len;
	char *payload;
	size_t payload_size;   /* total expected payload length */
	size_t payload_len;    /* amount read so far */
	size_t payload_alloc;
	uint32_t msg_type;
	int header_done;
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void ipc_socket_init(void);
void ipc_socket_finish(void);
void ipc_socket_send_workspace_event(const char *change);
void ipc_socket_send_window_event(const char *change);
void ipc_socket_send_output_event(void);

