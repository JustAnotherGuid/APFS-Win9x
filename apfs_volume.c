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
 * apfs_volume.c -- APFS volume enumeration and open
 *
 * Opens a volume within an APFS container by resolving the volume
 * OID through the container object map, reading the volume superblock,
 * and locating the file-system B-tree root.
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_volume_open -- Open an APFS volume within a container.
 *
 * Finds the vol_index-th valid volume OID in the container superblock's
 * nx_fs_oid[] array, resolves it through the container object map to
 * get the physical block address, reads and validates the volume
 * superblock, reads the volume object map, and resolves the FS B-tree
 * root OID through the volume omap.
 *
 * vol:        pointer to volume context. On entry, the container must
 *             already be open (container_sb, container_omap_root,
 *             block_size, max_xid populated by apfs_container_open).
 *             On success, the following fields are populated:
 *               vol->volume_sb         -- volume superblock
 *               vol->volume_omap_root  -- physical block of volume omap root
 *               vol->fs_tree_root      -- physical block of FS B-tree root
 *               vol->vol_label         -- UTF-8 volume name
 *               vol->vol_label_uni     -- UCS-2 Unicode volume name
 *               vol->vol_serial        -- synthetic serial number
 *
 * vol_index:  index of the volume to open (0 = first valid volume)
 *
 * Returns: 0 on success, -1 on failure
 */
int apfs_volume_open(apfs_volume_t* vol, uint32_t vol_index) {
	uint64_t vol_oid;
	uint32_t found;
	uint32_t i;
	uint64_t paddr;
	void* buf;
	apfs_superblock_t* vsb;
	uint64_t vol_omap_oid;
	void* omap_buf;
	omap_phys_t* omap;
	uint64_t fs_root_oid;
	uint32_t name_len;
	const uint8_t* uuid;

	if (vol == NULL) {
		return -1;
	}


	/* 
	 * The nx_fs_oid[] array has 100 entries. Non-zero entries are    
	 * valid volume OIDs. We skip zeros and count valid entries until 
	 * we reach the requested index.                                    
     */

	vol_oid = 0;
	found = 0;
	for (i = 0; i < 100; i++) {
		if (vol->container_sb.nx_fs_oid[i] == 0) continue;
		if (found == vol_index) {
			vol_oid = vol->container_sb.nx_fs_oid[i];
			break;
		}
		found++;
	}

	if (vol_oid == 0) {
		Debug_Printf("APFS: volume: volume index %lu not found\r\n", (DWORD)vol_index);
		return -1;
	}

	Debug_Printf("APFS: volume: opening volume index %lu, "
	             "OID 0x%08lX%08lX\r\n",
	             (DWORD)vol_index,
	             (DWORD)(vol_oid >> 32),
	             (DWORD)(vol_oid & 0xFFFFFFFF));


	if (apfs_omap_lookup(vol, vol->container_omap_root,
	                     vol_oid, vol->max_xid, &paddr) != 0) {
		Debug_Printf("APFS: volume: cannot resolve volume OID "
		             "0x%08lX%08lX\r\n",
		             (DWORD)(vol_oid >> 32),
		             (DWORD)(vol_oid & 0xFFFFFFFF));
		return -1;
	}

	buf = (void*)_HeapAllocate(vol->block_size, 0);
	if (buf == NULL) {
		Debug_Printf("APFS: volume: out of memory reading volume superblock\r\n");
		return -1;
	}

	if (apfs_read_block(vol, paddr, buf) != 0) {
		Debug_Printf("APFS: volume: cannot read volume superblock "
		             "at block 0x%08lX%08lX\r\n",
		             (DWORD)(paddr >> 32),
		             (DWORD)(paddr & 0xFFFFFFFF));
		_HeapFree(buf, 0);
		return -1;
	}

	vsb = (apfs_superblock_t*)buf;
	if (vsb->apfs_magic != APFS_MAGIC) {
		Debug_Printf("APFS: volume: invalid magic 0x%08lX "
		             "(expected 0x%08lX)\r\n",
		             (DWORD)vsb->apfs_magic,
		             (DWORD)APFS_MAGIC);
		_HeapFree(buf, 0);
		return -1;
	}

	if (apfs_verify_block(buf, vol->block_size) != 0) {
		Debug_Printf("APFS: volume: superblock checksum invalid\r\n");
		_HeapFree(buf, 0);
		return -1;
	}

	memcpy(&vol->volume_sb, vsb, sizeof(apfs_superblock_t));
	_HeapFree(buf, 0);
	buf = NULL;
	vsb = NULL;

	vol_omap_oid = vol->volume_sb.apfs_omap_oid;
	omap_buf = (void*)_HeapAllocate(vol->block_size, 0);
	if (omap_buf == NULL) {
		Debug_Printf("APFS: volume: out of memory reading volume omap\r\n");
		return -1;
	}

	if (apfs_read_block(vol, vol_omap_oid, omap_buf) != 0) {
		Debug_Printf("APFS: volume: cannot read volume omap at "
		             "block 0x%08lX%08lX\r\n",
		             (DWORD)(vol_omap_oid >> 32),
		             (DWORD)(vol_omap_oid & 0xFFFFFFFF));
		_HeapFree(omap_buf, 0);
		return -1;
	}

	if (apfs_verify_block(omap_buf, vol->block_size) != 0) {
		Debug_Printf("APFS: volume: omap checksum invalid\r\n");
		_HeapFree(omap_buf, 0);
		return -1;
	}

	omap = (omap_phys_t*)omap_buf;
	vol->volume_omap_root = omap->om_tree_oid;
	_HeapFree(omap_buf, 0);
	omap_buf = NULL;
	omap = NULL;

	fs_root_oid = vol->volume_sb.apfs_root_tree_oid;
	if (apfs_omap_lookup(vol, vol->volume_omap_root,
	                     fs_root_oid, vol->volume_sb.apfs_o.o_xid,
	                     &vol->fs_tree_root) != 0) {
		Debug_Printf("APFS: volume: cannot resolve FS B-tree root "
		             "OID 0x%08lX%08lX\r\n",
		             (DWORD)(fs_root_oid >> 32),
		             (DWORD)(fs_root_oid & 0xFFFFFFFF));
		return -1;
	}

	Debug_Printf("APFS: volume: vol name :%s\r\n", vol->volume_sb.apfs_volname);
	Debug_Printf("APFS: volume: num of files: %ld\r\n", vol->volume_sb.apfs_num_files);
	Debug_Printf("APFS: volume: num of dirs: %ld\r\n", vol->volume_sb.apfs_num_directories);

	memcpy(vol->vol_label, vol->volume_sb.apfs_volname, 256);
	vol->vol_label[255] = '\0';  /* ensure null termination */

	name_len = (uint32_t)strlen(vol->vol_label);
	if (name_len > 0) {
		BCSToUni((string_t)vol->vol_label_uni, (PBYTE)vol->vol_label, name_len, BCS_WANSI);
		/* Null-terminate the Unicode string */
		vol->vol_label_uni[name_len] = 0;
	} else {
		vol->vol_label_uni[0] = 0;
	}

	uuid = vol->volume_sb.apfs_vol_uuid;
	vol->vol_serial = (uint32_t)uuid[0] |
	                  ((uint32_t)uuid[1] << 8) |
	                  ((uint32_t)uuid[2] << 16) |
	                  ((uint32_t)uuid[3] << 24);

	Debug_Printf("APFS: volume: opened \"%s\", "
	             "omap_root=0x%08lX%08lX, "
	             "fs_root=0x%08lX%08lX, "
	             "serial=0x%08lX\r\n",
	             vol->vol_label,
	             (DWORD)(vol->volume_omap_root >> 32),
	             (DWORD)(vol->volume_omap_root & 0xFFFFFFFF),
	             (DWORD)(vol->fs_tree_root >> 32),
	             (DWORD)(vol->fs_tree_root & 0xFFFFFFFF),
	             (DWORD)vol->vol_serial);

	return 0;
}
