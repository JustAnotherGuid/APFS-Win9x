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
 * apfs_gpt.c -- GPT partition table parsing
 *
 * Reads the GPT header and partition entry array to locate
 * APFS partitions on USB mass storage devices.
 *
 * The GPT header lives at LBA 1 (byte offset 512). Since the volume's
 * block_size is set to 4096 before this function is called, reading
 * block 0 gives us sectors 0-7 (bytes 0-4095), which contains both
 * the protective MBR and the GPT header.
 *
 * Partition entries start at the LBA specified in the GPT header
 * (typically LBA 2 = byte offset 1024). We compute how many 4096-byte
 * blocks the entry array spans, allocate a heap buffer, read those
 * blocks, and scan for the APFS partition type GUID.
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define SECTOR_SIZE             512
#define GPT_HEADER_LBA          1       /* GPT header is at LBA 1 */
#define GPT_HEADER_BYTE_OFFSET  (GPT_HEADER_LBA * SECTOR_SIZE)

/* ========================================================================== */
/* Static helpers                                                             */
/* ========================================================================== */

/*
 * guid_match -- Compare two 16-byte GUIDs for equality.
 *
 * Returns non-zero if the GUIDs match, zero otherwise.
 */
static int guid_match(const uint8_t* a, const uint8_t* b) {
	return memcmp(a, b, 16) == 0;
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_gpt_find_partition -- Scan GPT for an APFS partition.
 *
 * Reads the GPT header from block 0 (at byte offset 512 within the
 * block), validates the "EFI PART" signature, then reads the partition
 * entry array and scans for the APFS partition type GUID. If found,
 * stores the partition's byte offset in *offset_out.
 *
 * In the VxD environment there is no user interaction, so if multiple
 * APFS partitions are found we simply use the first one.
 *
 * vol:        pointer to volume context (block_size must be set,
 *             partition_offset should be 0 for raw device reads)
 * offset_out: receives the byte offset of the APFS partition on disk
 * Returns:    0 if APFS partition found, -1 on error or not found
 */
int apfs_gpt_find_partition(apfs_volume_t* vol, uint64_t* offset_out) {
	uint8_t* block_buf;
	gpt_header_t* hdr;
	uint32_t num_entries;
	uint32_t entry_size;
	uint64_t array_byte_offset;
	uint64_t array_size;
	uint64_t first_block;
	uint64_t last_block;
	uint32_t num_blocks;
	uint8_t* entries_buf;
	uint32_t i;
	gpt_partition_entry_t* entry;
	uint64_t entry_byte_offset;
	int rc;

	if (vol == NULL || offset_out == NULL) return -1;
	block_buf = (uint8_t*) _HeapAllocate(vol->block_size, 0);
	if (block_buf == NULL) {
		Debug_Printf("APFS: gpt: out of memory for block buffer\r\n");
		return -1;
	}

	rc = apfs_read_block(vol, 0, block_buf);
	if (rc != 0) {
		Debug_Printf("APFS: gpt: cannot read block 0 (rc=%d)\r\n", rc);
		_HeapFree(block_buf, 0);
		return -1;
	}

	if (vol->block_size < GPT_HEADER_BYTE_OFFSET + sizeof(gpt_header_t)) {
		Debug_Printf("APFS: gpt: block_size %lu too small for GPT header\r\n",
		             (DWORD) vol->block_size);
		_HeapFree(block_buf, 0);
		return -1;
	}

	hdr = (gpt_header_t*)(block_buf + GPT_HEADER_BYTE_OFFSET);
	if (hdr->signature != GPT_SIGNATURE) {
		Debug_Printf("APFS: gpt: no GPT signature found "
		             "(got 0x%08lX%08lX)\r\n",
		             (DWORD)(hdr->signature >> 32),
		             (DWORD)(hdr->signature & 0xFFFFFFFF));
		_HeapFree(block_buf, 0);
		return -1;
	}


	if (hdr->size_of_partition_entry < sizeof(gpt_partition_entry_t)) {
		Debug_Printf("APFS: gpt: partition entry size %lu too small\r\n", (DWORD) hdr->size_of_partition_entry);
		_HeapFree(block_buf, 0);
		return -1;
	}

	Debug_Printf("APFS: gpt: signature OK, %lu entries, entry_size=%lu, "
	             "entry_lba=%lu\r\n",
	             (DWORD) hdr->num_partition_entries,
	             (DWORD) hdr->size_of_partition_entry,
	             (DWORD) hdr->partition_entry_lba);


	num_entries = hdr->num_partition_entries;
	entry_size  = hdr->size_of_partition_entry;
	array_byte_offset = hdr->partition_entry_lba * SECTOR_SIZE;
	array_size = (uint64_t) num_entries * (uint64_t) entry_size;

	_HeapFree(block_buf, 0);
	block_buf = NULL;
	hdr = NULL;

	if (array_size == 0) {
		Debug_Printf("APFS: gpt: partition entry array is empty\r\n");
		return -1;
	}


	first_block = array_byte_offset / vol->block_size;
	last_block  = (array_byte_offset + array_size - 1) / vol->block_size;
	num_blocks  = (uint32_t)(last_block - first_block + 1);

	entries_buf = (uint8_t*) _HeapAllocate(num_blocks * vol->block_size, 0);
	if (entries_buf == NULL) {
		Debug_Printf("APFS: gpt: out of memory for %lu entry blocks\r\n",
		             (DWORD) num_blocks);
		return -1;
	}

	rc = apfs_read_blocks(vol, first_block, num_blocks, entries_buf);
	if (rc != 0) {
		Debug_Printf("APFS: gpt: cannot read partition entry blocks "
		             "(rc=%d)\r\n", rc);
		_HeapFree(entries_buf, 0);
		return -1;
	}

	for (i = 0; i < num_entries; i++) {
		/*
		 * Compute the byte offset of this entry within our read buffer.
		 * The entry's absolute byte offset on disk is: array_byte_offset + i * entry_size
		 * Our buffer starts at: first_block * block_size
		 * So the offset within the buffer is: (array_byte_offset + i * entry_size) - (first_block * block_size)
		 */
		entry_byte_offset = (array_byte_offset + (uint64_t) i * entry_size) - (first_block * vol->block_size);
		entry = (gpt_partition_entry_t*)(entries_buf + (uint32_t) entry_byte_offset);

		if (!guid_match(entry->partition_type_guid, APFS_GPT_GUID)) continue;
	
		/* Found an APFS partition -- use the first one */
		*offset_out = entry->starting_lba * SECTOR_SIZE;

		Debug_Printf("APFS: gpt: found APFS partition %lu, "
		             "start_lba=%lu, offset=0x%08lX%08lX\r\n",
		             (DWORD) i,
		             (DWORD) entry->starting_lba,
		             (DWORD)(*offset_out >> 32),
		             (DWORD)(*offset_out & 0xFFFFFFFF));

		_HeapFree(entries_buf, 0);
		return 0;
	}

	Debug_Printf("APFS: gpt: no APFS partition found in %lu entries\r\n", (DWORD) num_entries);
	_HeapFree(entries_buf, 0);
	return -1;
}
