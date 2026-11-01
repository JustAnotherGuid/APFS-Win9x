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
 * apfs_checksum.c -- Fletcher-64 checksum for APFS blocks
 *
 * Computes and verifies the Fletcher-64 checksum stored in the
 * first 8 bytes of every APFS on-disk object.
 *
 * The APFS Fletcher-64 checksum works as follows:
 * - The block is treated as an array of 32-bit unsigned integers.
 * - Two 64-bit accumulators (sum1, sum2) are maintained.
 * - For each 32-bit word: sum1 += word, sum2 += sum1.
 * - After processing all words, the sums are folded:
 *     sum1 = sum1 % 0xFFFFFFFF
 *     sum2 = sum2 % 0xFFFFFFFF
 * - The checksum is computed so that when stored in the first 8 bytes
 *   of the object, the overall Fletcher-64 over the entire block is zero.
 *   Specifically: cksum_lo = 0xFFFFFFFF - ((sum1 + sum2) % 0xFFFFFFFF)
 *                 cksum_hi = 0xFFFFFFFF - ((sum1 + cksum_lo) % 0xFFFFFFFF)
 *                 result   = (cksum_hi << 32) | cksum_lo
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define MOD_VALUE 0xFFFFFFFF

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_fletcher64 -- Compute Fletcher-64 checksum over a buffer.
 *
 * The buffer is treated as an array of 32-bit words. Two 64-bit
 * accumulators are maintained and folded modulo 0xFFFFFFFF. The
 * result is constructed so that storing it in the first 8 bytes
 * of the block makes the overall checksum zero.
 */
uint64_t apfs_fletcher64(const void* buf, uint32_t len) {
	const uint32_t* data;
	uint32_t nwords;
	uint64_t sum1;
	uint64_t sum2;
	uint64_t cksum_lo;
	uint64_t cksum_hi;
	uint32_t i;

	data = (const uint32_t*)buf;
	nwords = len / sizeof(uint32_t);
	sum1 = 0;
	sum2 = 0;

	for (i = 0; i < nwords; i++) {
		sum1 = (sum1 + data[i]) % MOD_VALUE;
		sum2 = (sum2 + sum1) % MOD_VALUE;
	}

	cksum_lo = MOD_VALUE - ((sum1 + sum2) % MOD_VALUE);
	cksum_hi = MOD_VALUE - ((sum1 + cksum_lo) % MOD_VALUE);
	return (cksum_hi << 32) | cksum_lo;
}

/*
 * apfs_verify_block -- Verify Fletcher-64 checksum of an APFS object block.
 *
 * The checksum is stored in the first 8 bytes of the block (the o_cksum
 * field of obj_phys_t). We copy the block, zero the checksum field,
 * recompute, and compare against the stored value.
 *
 * Returns 0 on success, -1 on checksum mismatch or error.
 */
int apfs_verify_block(const void* block_buf, uint32_t block_size) {
	uint8_t* copy;
	uint64_t stored;
	uint64_t computed;
	const uint8_t* bytes;
	uint64_t oid;

	if (!block_buf || block_size < 8) return -1;

	/*
	 * The APFS Fletcher-64 checksum is stored in the first 8 bytes of the
	 * block. It is computed such that running apfs_fletcher64 over the
	 * entire block (with the checksum field zeroed) produces the stored value.
	 * So: zero out the checksum field, compute, compare.
	 */
	copy = (uint8_t*)_HeapAllocate(block_size, 0);
	if (!copy) {
		return -1;
	}

	memcpy(copy, block_buf, block_size);

	/* Save the stored checksum, then zero the field */
	memcpy(&stored, copy, sizeof(stored));
	memset(copy, 0, 8);

	/* Compute over the entire block with checksum field zeroed */
	computed = apfs_fletcher64(copy, block_size);
	_HeapFree(copy, 0);

	if (computed != stored) {
		/* Don't log errors for all-zero blocks (empty ring buffer slots) */
		if (stored == 0) {
			return -1;
		}
		bytes = (const uint8_t*)block_buf;
		memcpy(&oid, bytes + 8, sizeof(oid));
		Debug_Printf("APFS: checksum mismatch on object OID %lu (stored 0x%08lX%08lX, computed 0x%08lX%08lX)\n",
		             (unsigned long)oid,
		             (unsigned long)(stored >> 32),
		             (unsigned long)(stored & 0xFFFFFFFF),
		             (unsigned long)(computed >> 32),
		             (unsigned long)(computed & 0xFFFFFFFF));
		return -1;
	}

	return 0;
}
