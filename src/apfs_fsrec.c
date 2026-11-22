/*
 *     Copyright (C) 2026  Mustafa Ali
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * ========================================================================== 
 *
 * apfs_fsrec.c -- Inode, directory, and extent record operations
 *
 * Implements inode lookup, directory entry enumeration, file extent
 * enumeration, and data stream extraction from inode extended fields.
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Local macros                                                               */
/* ========================================================================== */

/* Round up to next multiple of 8 */
#define ALIGN8(x) (((x) + 7) & ~(uint16_t)7)

/* ========================================================================== */
/* Static helpers -- inode lookup                                              */
/* ========================================================================== */

/*
 * inode_lookup_ctx_t -- Visitor context for apfs_lookup_inode.
 *
 * Receives the first matching inode value record from the B-tree
 * enumeration and signals the visitor to stop.
 */
typedef struct {
	j_inode_val_t* out;
	uint16_t       out_len;
	int            found;
} inode_lookup_ctx_t;

/*
 * inode_visitor -- B-tree visitor callback for inode lookup.
 *
 * Copies the j_inode_val_t from the leaf record value into the
 * context output buffer. Returns 1 to stop enumeration after the
 * first match.
 */
static int inode_visitor(const void* key, uint16_t key_len,
                         const void* val, uint16_t val_len,
                         void* user_data) {
	inode_lookup_ctx_t* ctx;
	ctx = (inode_lookup_ctx_t*)user_data;
	if (val_len < sizeof(j_inode_val_t)) return 0;

	memcpy(ctx->out, val, sizeof(j_inode_val_t));
	ctx->out_len = val_len;
	ctx->found = 1;
	return 1;  /* stop after first match */
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_lookup_inode -- Look up an inode record by inode number.
 *
 * Enumerates the FS B-tree for APFS_TYPE_INODE records matching the
 * given inode number. The inode_visitor callback copies the first
 * matching j_inode_val_t into the output buffer.
 *
 * Returns 0 on success, -1 if not found or on error.
 */
int apfs_lookup_inode(apfs_volume_t* vol, uint64_t ino,
                      j_inode_val_t* out) {
	inode_lookup_ctx_t ctx;
	int rc;

	if (vol == NULL || out == NULL) return -1;

	ctx.out = out;
	ctx.out_len = 0;
	ctx.found = 0;

	rc = apfs_btree_enumerate(vol, vol->fs_tree_root, ino, APFS_TYPE_INODE, inode_visitor, &ctx);
	if (rc != 0) return -1;

	if (!ctx.found) {
		Debug_Printf("APFS: fsrec: inode 0x%08lX%08lX not found\r\n",
		             (DWORD)(ino >> 32),
		             (DWORD)(ino & 0xFFFFFFFF));
		return -1;
	}
	return 0;
}

/*
 * apfs_list_dir -- Enumerate directory entries for a directory inode.
 *
 * Simple wrapper around apfs_btree_enumerate that filters for
 * APFS_TYPE_DIR_REC records matching the given directory inode number.
 * The caller's visitor callback receives each DIR_REC key/value pair.
 *
 * Returns 0 on success, -1 on error.
 */
int apfs_list_dir(apfs_volume_t* vol, uint64_t dir_ino,
                  btree_visitor_fn visitor, void* user_data) {
	if (vol == NULL || visitor == NULL) return -1;

	return apfs_btree_enumerate(vol, vol->fs_tree_root, dir_ino, APFS_TYPE_DIR_REC, visitor, user_data);
}

/*
 * apfs_list_extents -- Enumerate file extent records for a file.
 *
 * Simple wrapper around apfs_btree_enumerate that filters for
 * APFS_TYPE_FILE_EXTENT records matching the given private_id
 * (from the inode record). The caller's visitor callback receives
 * each FILE_EXTENT key/value pair.
 *
 * Returns 0 on success, -1 on error.
 */
int apfs_list_extents(apfs_volume_t* vol, uint64_t private_id,
                      btree_visitor_fn visitor, void* user_data) {
	if (vol == NULL || visitor == NULL) return -1;
	return apfs_btree_enumerate(vol, vol->fs_tree_root, private_id, APFS_TYPE_FILE_EXTENT, visitor, user_data);
}

/*
 * apfs_extract_dstream -- Extract data stream info from inode xfields.
 *
 * Parses the inode extended fields (xfields) that follow the fixed
 * j_inode_val_t portion of the inode value record. Scans the
 * x_field_t header array looking for INO_EXT_TYPE_DSTREAM, then
 * skips through the data section to find the corresponding
 * j_dstream_t payload.
 *
 * Inode xfields layout (immediately after j_inode_val_t):
 *   x_fields_hdr_t        -- num_exts and used_data
 *   x_field_t headers[]   -- array of (type, flags, size) entries
 *   <padding to 8-byte alignment after all headers>
 *   field data[]           -- concatenated field data, each padded
 *                            to 8 bytes
 *
 * This is a pure data parsing function -- it does not need vol or
 * any OS services. It operates on the raw inode value buffer.
 *
 * Returns 0 on success, -1 if no dstream field found or on error.
 */
int apfs_extract_dstream(const void* val_buf, uint16_t val_len,
                         j_dstream_t* out) {
	const uint8_t* p;
	uint16_t remaining;
	const x_fields_hdr_t* xfh;
	uint16_t num_fields;
	const uint8_t* hdr_start;
	int target_index;
	uint16_t i;
	const x_field_t* xf;
	uint16_t hdr_total;
	uint16_t padded_hdr;
	uint16_t pad;
	int j;
	uint16_t field_size;

	if (val_buf == NULL || out == NULL) return -1;
	if (val_len <= sizeof(j_inode_val_t)) return -1;

	p = (const uint8_t*)val_buf + sizeof(j_inode_val_t);
	remaining = val_len - (uint16_t)sizeof(j_inode_val_t);
	if (remaining < sizeof(x_fields_hdr_t)) return -1;

	xfh = (const x_fields_hdr_t*)p;
	num_fields = xfh->xf_num_exts;
	p += sizeof(x_fields_hdr_t);
	remaining -= (uint16_t)sizeof(x_fields_hdr_t);

	/* First pass: scan xfield headers to find the dstream */
	hdr_start = p;
	target_index = -1;

	for (i = 0; i < num_fields; i++) {
		if (remaining < sizeof(x_field_t)) return -1;

		xf = (const x_field_t*)p;
		if (xf->x_type == INO_EXT_TYPE_DSTREAM) {
			target_index = (int)i;
		}

		p += sizeof(x_field_t);
		remaining -= (uint16_t)sizeof(x_field_t);
	}

	if (target_index < 0) return -1; /* no dstream field */

	/* Skip padding after headers to 8-byte alignment */
	hdr_total = (uint16_t)(p - hdr_start);
	padded_hdr = ALIGN8(hdr_total);
	pad = padded_hdr - hdr_total;

	if (pad > remaining) return -1;

	p += pad;
	remaining -= pad;

	/* Second pass: skip data fields until we reach target_index */
	xf = (const x_field_t*)hdr_start;
	for (j = 0; j < target_index; j++) {
		field_size = ALIGN8(xf[j].x_size);
		if (field_size > remaining) return -1;
		p += field_size;
		remaining -= field_size;
	}

	/* Read the dstream */
	if (remaining < sizeof(j_dstream_t)) return -1;
	memcpy(out, p, sizeof(j_dstream_t));
	return 0;
}
