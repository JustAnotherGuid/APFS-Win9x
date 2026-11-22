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
 * apfs_container.c -- APFS container superblock and checkpoint scanning
 *
 * Reads and validates the container superblock, scans the checkpoint
 * descriptor area for the most recent valid superblock, and locates
 * the container object map root.
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Static helpers                                                             */
/* ========================================================================== */

/*
 * scan_checkpoint_area -- Scan the checkpoint descriptor ring buffer for
 * the container superblock with the highest valid XID.
 *
 * The checkpoint descriptor area is a contiguous range of blocks starting
 * at nx_xp_desc_base with nx_xp_desc_blocks entries. Each block may
 * contain a checkpoint superblock (type NX_SUPERBLOCK). We scan all
 * blocks, verify checksums, and keep the one with the highest XID.
 *
 * Falls back to the block-zero superblock if no valid checkpoint
 * superblock is found (i.e., vol->container_sb is unchanged).
 *
 * vol:  pointer to volume context (container_sb already populated with
 *       the block-zero superblock)
 * sb0:  pointer to the block-zero superblock (for ring buffer params)
 * Returns: 0 on success, -1 on fatal error
 */
static int scan_checkpoint_area(apfs_volume_t* vol,
                                const nx_superblock_t* sb0) {
	uint64_t base;
	uint32_t count;
	void* buf;
	uint64_t best_xid;
	int found_better;
	uint32_t i;
	obj_phys_t* hdr;
	uint32_t obj_type;
	nx_superblock_t* candidate;

	base = sb0->nx_xp_desc_base;
	count = sb0->nx_xp_desc_blocks;

	/*
	 * The high bit of nx_xp_desc_blocks is a flag indicating whether
	 * the checkpoint descriptor area is stored in a B-tree (1) or
	 * contiguously (0). We only support contiguous for now.
	 */
	if (count & (1U << 31)) {
		Debug_Printf("APFS: container: non-contiguous checkpoint "
		             "descriptor area not supported, using "
		             "block-zero superblock\r\n");
		return 0;
	}
	if (count == 0) return 0; /* nothing to scan, keep sb0 */

	buf = (void*)_HeapAllocate(vol->block_size, 0);
	if (buf == NULL) {
		Debug_Printf("APFS: container: out of memory scanning checkpoint area\r\n");
		return -1;
	}

	best_xid = sb0->nx_o.o_xid;
	found_better = 0;
	for (i = 0; i < count; i++) {
		if (apfs_read_block(vol, base + i, buf) != 0) continue;
		if (apfs_verify_block(buf, vol->block_size) != 0) continue;

		hdr = (obj_phys_t*)buf;
		obj_type = hdr->o_type & OBJ_TYPE_MASK_TYPE;

		if (obj_type != OBJECT_TYPE_NX_SUPERBLOCK) continue;

		/* Verify this is actually a valid container superblock */
		candidate = (nx_superblock_t*)buf;
		if (candidate->nx_magic != NX_MAGIC) continue;

		if (hdr->o_xid > best_xid) {
			best_xid = hdr->o_xid;
			memcpy(&vol->container_sb, candidate, sizeof(nx_superblock_t));
			found_better = 1;
		}
	}

	_HeapFree(buf, 0);
	if (found_better) {
		Debug_Printf("APFS: container: selected checkpoint superblock with XID 0x%08lX%08lX\r\n",
		             (DWORD)(best_xid >> 32),
		             (DWORD)(best_xid & 0xFFFFFFFF));
	}

	return 0;
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_container_open -- Read and validate the APFS container superblock.
 *
 * Reads block 0, verifies NX_MAGIC and Fletcher-64 checksum, extracts
 * the block size, scans the checkpoint descriptor area for a newer
 * valid superblock, reads the container object map, and extracts the
 * omap B-tree root.
 *
 * vol:  pointer to volume context. On entry, vol->block_size must be
 *       set to 4096 (initial default) and vol->partition_offset must
 *       be set. On success, the following fields are populated:
 *         vol->block_size          -- container block size
 *         vol->container_sb        -- latest valid container superblock
 *         vol->container_omap_root -- physical block of omap B-tree root
 *         vol->max_xid             -- highest valid transaction ID
 *
 * Returns: 0 on success, -1 on failure
 */
int apfs_container_open(apfs_volume_t* vol) {
	void* buf;
	nx_superblock_t* sb;
	uint64_t omap_oid;
	void* omap_buf;
	omap_phys_t* omap;

	if (vol == NULL) {
		Debug_Printf("APFS: container: volume is null\r\n");
		return -1;
	}

	buf = (void*)_HeapAllocate(vol->block_size, 0);
	if (buf == NULL) {
		Debug_Printf("APFS: container: out of memory in apfs_container_open\r\n");
		return -1;
	}

	if (apfs_read_block(vol, 0, buf) != 0) {
		Debug_Printf("APFS: container: cannot read superblock at block 0\r\n");
		_HeapFree(buf, 0);
		return -1;
	}

	sb = (nx_superblock_t*)buf;
	if (sb->nx_magic != NX_MAGIC) {
		Debug_Printf("APFS: container: not a valid APFS container (magic 0x%08lX, expected 0x%08lX)\r\n",
		             (DWORD)sb->nx_magic, (DWORD)NX_MAGIC);
		_HeapFree(buf, 0);
		return -1;
	}

	if (apfs_verify_block(buf, vol->block_size) != 0) {
		Debug_Printf("APFS: container: superblock checksum invalid\r\n");
		_HeapFree(buf, 0);
		return -1;
	}

	vol->block_size = sb->nx_block_size;
	Debug_Printf("APFS: container: block_size=%lu, block_count=0x%08lX%08lX, xid=0x%08lX%08lX\r\n",
	             (DWORD)sb->nx_block_size,
	             (DWORD)(sb->nx_block_count >> 32),
	             (DWORD)(sb->nx_block_count & 0xFFFFFFFF),
	             (DWORD)(sb->nx_o.o_xid >> 32),
	             (DWORD)(sb->nx_o.o_xid & 0xFFFFFFFF));


	memcpy(&vol->container_sb, sb, sizeof(nx_superblock_t));
	vol->max_xid = sb->nx_o.o_xid;
	_HeapFree(buf, 0);
	buf = NULL;
	sb = NULL;
	if (scan_checkpoint_area(vol, &vol->container_sb) != 0) {
		return -1;
	}

	/* Update max_xid from the (possibly updated) container superblock */
	vol->max_xid = vol->container_sb.nx_o.o_xid;
	omap_oid = vol->container_sb.nx_omap_oid;

	omap_buf = (void*)_HeapAllocate(vol->block_size, 0);
	if (omap_buf == NULL) {
		Debug_Printf("APFS: container: out of memory reading container omap\r\n");
		return -1;
	}

	if (apfs_read_block(vol, omap_oid, omap_buf) != 0) {
		Debug_Printf("APFS: container: cannot read omap at block 0x%08lX%08lX\r\n",
		             (DWORD)(omap_oid >> 32),
		             (DWORD)(omap_oid & 0xFFFFFFFF));
		_HeapFree(omap_buf, 0);
		return -1;
	}

	if (apfs_verify_block(omap_buf, vol->block_size) != 0) {
		Debug_Printf("APFS: container: omap checksum invalid\r\n");
		_HeapFree(omap_buf, 0);
		return -1;
	}

	omap = (omap_phys_t*)omap_buf;
	vol->container_omap_root = omap->om_tree_oid;

	Debug_Printf("APFS: container: omap_root=0x%08lX%08lX, max_xid=0x%08lX%08lX\r\n",
	             (DWORD)(vol->container_omap_root >> 32),
	             (DWORD)(vol->container_omap_root & 0xFFFFFFFF),
	             (DWORD)(vol->max_xid >> 32),
	             (DWORD)(vol->max_xid & 0xFFFFFFFF));

	_HeapFree(omap_buf, 0);
	return 0;
}
