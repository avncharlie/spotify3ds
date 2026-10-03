#pragma once

#include <stdint.h>

#define ARTCACHE_MAGIC UINT32_C(0x43334100) /* "\0A3C" */

/* Version 4 layout. New quality bits use the existing flags field; generated
 * image timestamps use reserved. No payload or header-size migration needed. */
typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint16_t tex_dim;
	uint16_t src_w, src_h;
	uint8_t accent_r, accent_g, accent_b;
	uint32_t payload_len;
	uint32_t crc32;
	uint32_t use_seq; /* legacy field; eviction is shard-local FIFO */
	uint32_t reserved;
} artcache_hdr;

_Static_assert(sizeof(artcache_hdr) == 33, "artcache header layout changed");
