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
 * apfs_blockio.c -- IOR-based block I/O and LRU block cache
 *
 * Provides block read services using IOS I/O Request packets,
 * with a 64-entry LRU cache to reduce redundant disk reads
 * during B-tree traversal.
 *
 * All reads go through the cache: on a miss, IOS reads disk sectors
 * directly into a cache entry's heap buffer (already page-locked by
 * VMM), then we memcpy to the caller's buffer.
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Forward declarations                                                       */
/* ========================================================================== */

static void __cdecl apfs_on_block_read_complete(PIOR ior);

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define SECTOR_SIZE 512

/*
 * cache_init -- Allocate and initialize a block cache for a volume.
 *
 * Allocates the block_cache_t structure and one block_size buffer per
 * entry via _HeapAllocate. All entries are marked empty (block_addr
 * set to UINT64_MAX 0xFFFFFFFFFFFFFFFF - which is not possible on
 * real 32-bit systems.
 *
 * block_size: size of each cached block in bytes
 * Returns:    pointer to new block_cache_t, or NULL on failure
 */
block_cache_t* cache_init(uint32_t block_size) {
	block_cache_t* cache;
	uint32_t i;

	cache = (block_cache_t*) _HeapAllocate(sizeof(block_cache_t), HEAPZEROINIT);
	if (cache == NULL) {
		Debug_Printf("APFS: cache_init: out of memory for cache struct\r\n");
		return NULL;
	}

	cache->num_entries = BLOCK_CACHE_SIZE;
	cache->lru_counter = 0;
	cache->block_size  = block_size;

	for (i = 0; i < BLOCK_CACHE_SIZE; i++) {
		cache->entries[i].block_addr = ((uint64_t)0xFFFFFFFF << 32 | (uint64_t)0xFFFFFFFF);
		cache->entries[i].age        = 0;
		cache->entries[i].data       = (uint8_t*)_HeapAllocate(block_size, HEAPZEROINIT);
		if (cache->entries[i].data == NULL) {
			Debug_Printf("APFS: cache_init: out of memory for entry %lu\r\n", (DWORD) i);
			/* Free already-allocated entries and the cache struct */
			while (i > 0) {
				i--;
				_HeapFree(cache->entries[i].data, 0);
			}
			_HeapFree(cache, 0);
			return NULL;
		}
	}

	Debug_Printf("APFS: cache_init: %lu entries, block_size=%lu\r\n", (DWORD) BLOCK_CACHE_SIZE, (DWORD) block_size);
	return cache;
}

/*
 * cache_destroy -- Free all cache buffers and the cache structure.
 *
 * cache: pointer to block_cache_t (may be NULL)
 */
void cache_destroy(block_cache_t* cache) {
	uint32_t i;

	if (cache == NULL) return;
	for (i = 0; i < BLOCK_CACHE_SIZE; i++) {
		if (cache->entries[i].data != NULL) {
			_HeapFree(cache->entries[i].data, 0);
			cache->entries[i].data = NULL;
		}
	}

	_HeapFree(cache, 0);
	Debug_Printf("APFS: cache_destroy: cache freed\r\n");
}

/*
 * cache_find -- Look up a block in the cache.
 *
 * Linear scan for matching block_addr. On hit, updates the entry's
 * age counter (LRU touch) and returns a pointer to the cached data.
 *
 * cache:      pointer to block_cache_t
 * block_addr: APFS block address to find
 * Returns:    pointer to cached block data, or NULL if not cached
 */
uint8_t* cache_find(block_cache_t* cache, uint64_t block_addr) {
	uint32_t i;

	if (cache == NULL) return NULL;

	for (i = 0; i < BLOCK_CACHE_SIZE; i++) {
		if (cache->entries[i].block_addr == block_addr) {
			/* Cache hit -- update LRU age */
			cache->lru_counter++;
			cache->entries[i].age = cache->lru_counter;
			return cache->entries[i].data;
		}
	}

	return NULL;  /* cache miss */
}

/*
 * cache_invalidate_all -- Mark all cache entries as empty.
 *
 * Sets all block_addr fields to the UINT64_MAX value and resets
 * the LRU counter. Does not free the data buffers.
 *
 * cache: pointer to block_cache_t (may be NULL)
 */
void cache_invalidate_all(block_cache_t* cache) {
	uint32_t i;

	if (cache == NULL) return;
	for (i = 0; i < BLOCK_CACHE_SIZE; i++) {
		cache->entries[i].block_addr = ((uint64_t)0xFFFFFFFF << 32 | (uint64_t)0xFFFFFFFF);
		cache->entries[i].age = 0;
	}

	cache->lru_counter = 0;
	Debug_Printf("APFS: cache_invalidate_all: all entries cleared\r\n");
}


/* ========================================================================== */
/* Internal helpers                                                           */
/* ========================================================================== */

/*
 * cache_get_evict_buffer -- Get a cache entry buffer for a new block read.
 *
 * Finds the LRU entry (lowest age) or an empty slot and returns a
 * pointer to its data buffer. The caller reads disk data directly into
 * this buffer, then calls cache_commit_entry() to finalize the entry.
 *
 * This supports the cache-mediated read flow:
 * IOR_buffer_ptr points to heap-allocated cache memory (already locked
 * by VMM), so no _LinPageLock is needed.
 *
 * cache:      pointer to block_cache_t
 * out_index:  receives the index of the selected cache entry
 * Returns:    pointer to the entry's data buffer, or NULL if cache is NULL
 */
static uint8_t* cache_get_evict_buffer(block_cache_t* cache, uint32_t* out_index) {
	uint32_t i;
	uint32_t lru_idx;
	uint32_t lru_age;

	if (cache == NULL) return NULL;

	/* Find the LRU entry (lowest age) or first empty slot */
	lru_idx = 0;
	lru_age = cache->entries[0].age;

	for (i = 1; i < BLOCK_CACHE_SIZE; i++) {
		/* Prefer empty entries */
		if (cache->entries[i].block_addr == ((uint64_t)0xFFFFFFFF << 32 | (uint64_t)0xFFFFFFFF)) {
			lru_idx = i;
			break;
		}
		if (cache->entries[i].age < lru_age) {
			lru_age = cache->entries[i].age;
			lru_idx = i;
		}
	}

	*out_index = lru_idx;
	return cache->entries[lru_idx].data;
}

/*
 * cache_commit_entry -- Finalize a cache entry after a successful read.
 *
 * Sets the block_addr and updates the LRU age for the entry that was
 * previously selected by cache_get_evict_buffer().
 *
 * cache:      pointer to block_cache_t
 * index:      cache entry index (from cache_get_evict_buffer)
 * block_addr: APFS block address that was read into this entry
 */
static void cache_commit_entry(block_cache_t* cache, uint32_t index, uint64_t block_addr) {
	if (cache == NULL) return;

	cache->entries[index].block_addr = block_addr;
	cache->lru_counter++;
	cache->entries[index].age = cache->lru_counter;
}



/*
 * apfs_read_block -- Read a single APFS block via IOS, with caching.
 *
 * Flow is as follows:
 *   1. Check cache -- if hit, memcpy and return
 *   2. Compute sector address from partition offset and block address
 *   3. Allocate IOR from VRP parameters
 *   4. Initialize IOR fields (buffer_ptr = cache entry, not user buf)
 *   5. Check I/O criteria (IlbIoCriteria)
 *   6. Handle partition offset for removable media
 *   7. Set up async completion callback
 *   8. Send command via IOS_SendCommand
 *   9. Wait for completion (IFSMgr_Block )
 *  10. Check status and map errors
 *  11. Destroy IOR
 *  12. On success, commit cache entry and memcpy to user buf
 *  13. Return error code
 *
 * vol:        pointer to mounted volume context
 * block_addr: APFS block address to read
 * buf:        output buffer (must be at least vol->block_size bytes)
 * Returns:    0 on success, Win32 error code on failure
 */
int apfs_read_block(apfs_volume_t* vol, uint64_t block_addr, void* buf) {
	PVRP vrp;
	PDCB dcb;
	PIOR ior;
	USHORT iop_size;
	ULONG delta;
	DWORD sector;
	DWORD count;
	DWORD criteria;
	DWORD status;
	DWORD error;
	uint8_t* cached;
	uint8_t* read_buf;
	uint32_t cache_idx;

	if (vol == NULL || buf == NULL) return ERROR_INVALID_PARAMETER;
	cached = cache_find(vol->cache, block_addr);
	if (cached != NULL) {
		memcpy(buf, cached, vol->block_size);
		return 0;
	}


	vrp = vol->vrp;
	dcb = (PDCB) vrp->VRP_device_handle;
	sector = (DWORD)(vol->partition_offset / SECTOR_SIZE) + (DWORD)(block_addr * (vol->block_size / SECTOR_SIZE));
	count  = vol->block_size / SECTOR_SIZE;

	read_buf = cache_get_evict_buffer(vol->cache, &cache_idx);
	if (read_buf == NULL) {
		read_buf = (uint8_t*) buf;
		cache_idx = 0xFFFFFFFF;  /* No cache commit */
	}


	iop_size = vrp->VRP_max_req_size + vrp->VRP_max_sgd * sizeof(SGD);
	delta    = vrp->VRP_delta_to_ior;

	ior = IspCreateIor(iop_size, delta, 0);
	if (ior == NULL) {
		Debug_Printf("APFS: read_block: cannot allocate IOR\r\n");
		return ERROR_NOT_ENOUGH_MEMORY;
	}

	ior->IOR_next            = 0;
	ior->IOR_func            = IOR_READ;
	ior->IOR_flags           = IORF_VERSION_002 | IORF_BYPASS_VOLTRK
	                           | IORF_HIGH_PRIORITY;
	ior->IOR_start_addr[0]   = sector;
	ior->IOR_start_addr[1]   = 0;
	ior->IOR_xfer_count      = count;
	ior->IOR_buffer_ptr      = (ULONG) read_buf;
	ior->IOR_num_sgds        = 0;
	ior->IOR_vol_designtr    = vol->drive;
	ior->IOR_req_vol_handle  = (ULONG) vrp;
	ior->IOR_private_client  = delta;
	ior->IOR_sgd_lin_phys    = (ULONG)(ior + 1);

	criteria = IlbIoCriteria(ior);
	if (criteria != 0) {
		ior->IOR_flags |= IORF_DOUBLE_BUFFER;
	}


	if (vrp->VRP_demand_flags & VRP_dmd_removable_supp) {
		ior->IOR_flags &= ~IORF_LOGICAL_START_SECTOR;
		ior->IOR_start_addr[0] += vrp->VRP_partition_offset;
	} else {
		ior->IOR_flags |= IORF_LOGICAL_START_SECTOR;
	}

	ior->IOR_completion_flag = 0;
	ior->IOR_callback        = (CMDCPLT) apfs_on_block_read_complete;
	ior->IOR_req_req_handle  = (ULONG) ior;


	/* IOS may clobber EDI -- save/restore */
	_asm push edi
	IOS_SendCommand(ior, dcb);
	_asm pop edi

	/*
	 * Although the DDK sample and LPFS from Systems Programming for Windows 95
	 * wrap these calls around cli/sti blocks to prevent interrupts we do not
	 * do that here because IFSMgr_Block wraps around the VMM's __BlockOnId 
	 * function with interrupts enabled so cli would kind of defeat the purpose.
	 */
	while (!ior->IOR_completion_flag) {
		IFSMgr_Block((DWORD) &ior->IOR_completion_flag);
	}


	status = ior->IOR_status;
	if (status >= IORS_ERROR_DESIGNTR) {
		error = IOSMapIORSToI21(status);
		Debug_Printf("APFS: read_block: IOR failed, status=0x%04lX err=%lu "
		             "block=%lu\r\n", status, error, (DWORD) block_addr);
	} else {
		error = 0;
	}


	IspDeallocMem((PVOID)((DWORD)ior - ior->IOR_private_client));
	if (error == 0) {
		if (cache_idx != 0xFFFFFFFF) {
			cache_commit_entry(vol->cache, cache_idx, block_addr);
			memcpy(buf, read_buf, vol->block_size);
		}
	}
	return (int) error;
}


/*
 * apfs_read_blocks -- Read multiple contiguous APFS blocks via IOS.
 *
 * vol:        pointer to mounted volume context
 * block_addr: starting APFS block address
 * count:      number of blocks to read
 * buf:        output buffer (must be at least count * vol->block_size bytes)
 * Returns:    0 on success, Win32 error code on first failure
 */
int apfs_read_blocks(apfs_volume_t* vol, uint64_t block_addr, uint32_t count, void* buf) {
	uint32_t i;
	int rc;
	uint8_t* dst;

	if (vol == NULL || buf == NULL) return ERROR_INVALID_PARAMETER;
	if (count == 0) return 0;

	dst = (uint8_t*) buf;
	for (i = 0; i < count; i++) {
		rc = apfs_read_block(vol, block_addr + i, dst);
		if (rc != 0) {
			Debug_Printf("APFS: read_blocks: block %lu failed (rc=%d)\r\n", (DWORD)(block_addr + i), rc);
			return rc;
		}
		dst += vol->block_size;
	}

	return 0;
}


/*
 * apfs_on_block_read_complete -- IOR completion callback.
 *
 * Uses __cdecl calling convention as required for IOS callbacks.
 *
 * ior: pointer to the completed I/O Request
 */
static void __cdecl apfs_on_block_read_complete(PIOR ior) {
	ior->IOR_completion_flag = 1;
	IFSMgr_Wakeup((DWORD) &ior->IOR_completion_flag);
}
