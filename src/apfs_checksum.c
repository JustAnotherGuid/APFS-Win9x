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
 * - Two 32-bit values (sum1, sum2) that are added and modulo-ed with 0xFFFFFFFF.
 * - For each 32-bit word: sum1 += word, sum2 += sum1.
 * - After processing all words, the sums are combined:
 *     cksum_lo = MOD_VALUE - addmod(sum1, sum2)
 *     cksum_hi = MOD_VALUE - addmod(sum1, cksum_lo)
 *     (cksum_hi << 32) | cksum_lo
 * - Because checksum is stored in the first 8 bytes of any given block, when it
 *   is calculated we ignore the first uint64_t/first two words.
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define MOD_VALUE 0xFFFFFFFF

/* ========================================================================== */
/* Static Helpers                                                             */
/* ========================================================================== */

/*
 * This function is takes two values, adding them and then modulo-ing
 * them with 0xFFFFFFFF without using __aullrem. The idea is that when
 * there is an overflow we add one the the jumps off the end of the 
 * value which gives us (a + b) % 0xFFFFFFFF.
 */
static uint32_t addmod(uint32_t a, uint32_t b) {
	uint32_t c = a + b;
	if(c < a) c++;	/* The integer value has overflown so add 1 back */

	/*
	 * This is for the case where result is exactly the number we 
	 * we want to modulo the sum by so we just return 0 here.
	 */
	if(c == 0xFFFFFFFF) return 0; 
	return c;
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_fletcher64 -- Compute Fletcher-64 checksum over a buffer.
 *
 * The buffer is treated as an array of 32-bit words. Two 32-bit
 * accumulators are maintained and folded modulo 0xFFFFFFFF. The
 * result is constructed so that storing it in the first 8 bytes
 * of the block makes the overall checksum zero.
 */
uint64_t apfs_fletcher64(const void* buf, uint32_t len, uint32_t first) {
	const uint32_t* data;
	uint32_t nwords;
	uint32_t sum1;
	uint32_t sum2;
	uint32_t cksum_lo;
	uint32_t cksum_hi;
	uint32_t i;

	data = (const uint32_t*)buf;
	nwords = len / sizeof(uint32_t);
	sum1 = 0;
	sum2 = 0;

	for (i = first; i < nwords; i++) {
		sum1 = addmod(sum1, data[i]);
		sum2 = addmod(sum2, sum1);
	}

	cksum_lo = MOD_VALUE - addmod(sum1, sum2);
	cksum_hi = MOD_VALUE - addmod(sum1, cksum_lo);
	return (((uint64_t)cksum_hi) << 32) | cksum_lo;
}

/*
 * apfs_verify_block -- Verify Fletcher-64 checksum of an APFS object block.
 *
 * The checksum is stored in the first 8 bytes of the block (the o_cksum
 * field of obj_phys_t). We copy the checksum, recompute, and compare against
 * the stored value.
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
	memcpy(&stored, block_buf, sizeof(stored));
	computed = apfs_fletcher64(block_buf, block_size, 2); /* Jump over the current checksum */
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
