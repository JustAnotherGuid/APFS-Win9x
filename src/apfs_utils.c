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
 * apfs_utils.c -- Generic helper functions used by multiple modules
 *
 */
#include "apfs_vxd.h"


/*
 * fill_find_data_local -- Fill a _WIN32_FIND_DATA structure from a
 * cached directory entry.
 */
void fill_find_data(_WIN32_FIND_DATA* fdp, dir_entry_cache_t* entry) {
	_FILETIME ft;
	uint16_t copy_len;

	memset(fdp, 0, sizeof(_WIN32_FIND_DATA));

	fdp->dwFileAttributes = entry->attr;

	/* Convert DOS timestamps to Win32 FILETIME */
	ft = IFSMgr_DosToWin32Time(entry->created);
	fdp->ftCreationTime = ft;

	ft = IFSMgr_DosToWin32Time(entry->modified);
	fdp->ftLastWriteTime = ft;

	ft = IFSMgr_DosToWin32Time(entry->accessed);
	fdp->ftLastAccessTime = ft;

	/* File size (split into high/low 32-bit parts) */
	fdp->nFileSizeLow  = (DWORD)(entry->size & 0xFFFFFFFF);
	fdp->nFileSizeHigh = (DWORD)(entry->size >> 32);

	/* Copy long filename (Unicode) */
	copy_len = entry->name_len;
	if (copy_len > (MAX_PATH - 1)) {
		copy_len = MAX_PATH - 1;
	}
	memcpy(fdp->cFileName, entry->name, copy_len * sizeof(USHORT));

	/* Copy 8.3 alternate filename */
	memcpy(fdp->cAlternateFileName, entry->short_name, 13 * sizeof(USHORT));
}

/*
 * inode_raw_ctx_t -- Visitor context for looking up an inode and
 * capturing the raw value buffer (needed for dstream extraction).
 */
typedef struct {
	j_inode_val_t*  out;
	uint8_t*        raw_buf;
	uint16_t        raw_buf_size;
	uint16_t        raw_len;
	int             found;
} inode_raw_ctx_t;

/*
 * inode_raw_visitor -- B-tree visitor that captures the full raw
 * inode value record (including xfields) for dstream extraction.
 */
static int inode_raw_visitor(const void* key, uint16_t key_len,
                             const void* val, uint16_t val_len,
                             void* user_data) {
	inode_raw_ctx_t* ctx;
	uint16_t copy_len;

	ctx = (inode_raw_ctx_t*)user_data;
	if (val_len < sizeof(j_inode_val_t)) return 0;

	memcpy(ctx->out, val, sizeof(j_inode_val_t));

	/* Copy the full raw value for dstream extraction */
	copy_len = val_len;
	if (copy_len > ctx->raw_buf_size) {
		copy_len = ctx->raw_buf_size;
	}
	memcpy(ctx->raw_buf, val, copy_len);
	ctx->raw_len = val_len;
	ctx->found = 1;
	return 1;  /* stop after first match */
}


/*
 * apfs_inode_info -- Look up an inode and return its logical size.
 *
 * Unlike apfs_lookup_inode(), this captures the full inode value
 * (including extended fields) and extracts the DSTREAM xfield to obtain
 * the real file size. In APFS the fixed inode's uncompressed_size field
 * is 0 for normal files -- the authoritative size lives in the dstream
 * (this mirrors the macOS reference reader's file_extract path).
 *
 * out_inode (optional) receives the fixed j_inode_val_t.
 * out_size receives the logical size (0 for directories).
 * Returns 0 on success, -1 if the inode was not found.
 */
int apfs_inode_info(apfs_volume_t* vol, uint64_t ino,
                           j_inode_val_t* out_inode, uint64_t* out_size) {
	j_inode_val_t inode;
	uint8_t raw_buf[512];
	inode_raw_ctx_t raw_ctx;
	j_dstream_t dstream;
	int rc;

	if (out_size != NULL) {
		*out_size = 0;
	}

	raw_ctx.out = &inode;
	raw_ctx.raw_buf = raw_buf;
	raw_ctx.raw_buf_size = sizeof(raw_buf);
	raw_ctx.raw_len = 0;
	raw_ctx.found = 0;

	rc = apfs_btree_enumerate(vol, vol->fs_tree_root, ino, APFS_TYPE_INODE, inode_raw_visitor, &raw_ctx);
	if (rc != 0 || !raw_ctx.found) return -1;

	if (out_inode != NULL) {
		memcpy(out_inode, &inode, sizeof(j_inode_val_t));
	}

	/* Directories carry no data stream. Detect via the inode mode
	 * (S_IFDIR == 0x4000), independent of the drec d_type. */
	if ((inode.mode & 0xF000) == 0x4000) return 0;

	memset(&dstream, 0, sizeof(dstream));
	if (raw_ctx.raw_len > sizeof(j_inode_val_t) && apfs_extract_dstream(raw_buf, raw_ctx.raw_len, &dstream) == 0) {
		if (out_size != NULL) {
			*out_size = dstream.size;
		}
		return 0;
	}

	/* Fallback: the fixed inode's uncompressed_size (usually 0). */
	if (out_size != NULL) {
		*out_size = inode.uncompressed_size;
	}
	return 0;
}
