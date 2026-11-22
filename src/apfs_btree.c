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
 * apfs_btree.c -- Generic APFS B-tree traversal
 *
 * Implements lookup and enumeration over APFS B-tree nodes,
 * handling fixed-size and variable-size key-value layouts,
 * leaf and internal nodes, and omap-resolved child pointers.
 *
 * Two public entry points:
 *   apfs_btree_lookup()          -- FS tree lookups (virtual, uses vol omap)
 *   apfs_btree_lookup_physical() -- omap tree lookups (physical, no omap)
 *   apfs_btree_enumerate()       -- enumerate by OID + record type
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Internal helpers                                                           */
/* ========================================================================== */

/*
 * omap_key_compare -- Compare two APFS OMAP B-tree keys for internal-node descent.
 *
 * APFS keys (omap_key_t {oid,xid}, and the leading j_key_t word) are
 * little-endian 64-bit integers, and the trees are sorted NUMERICALLY by
 * those integers.  We compare 64-bit little-endian words numerically   
 * across the common prefix (min of the two lengths). This is used only
 * for prefix-based descent.
 *
 * Returns negative if a < b, positive if a > b, zero if the compared
 * prefix is equal.
 */
static int omap_key_compare(const void* a, uint16_t a_len,
                       const void* b, uint16_t b_len) {
	const uint8_t* pa = (const uint8_t*)a;
	const uint8_t* pb = (const uint8_t*)b;
	uint16_t min_len;
	uint16_t off;

	min_len = a_len < b_len ? a_len : b_len;

	/* Compare whole 64-bit little-endian words numerically.
	 * Note this code will only work for powers of 8 values.
	 */
	for (off = 0; (uint16_t)(off + sizeof(uint64_t)) <= min_len; off += sizeof(uint64_t)) {
		uint64_t va, vb;
		memcpy(&va, pa + off, sizeof(uint64_t));
		memcpy(&vb, pb + off, sizeof(uint64_t));
		if (va < vb) return -1;
		if (va > vb) return 1;
	}
	return 0;
}

/*
 * resolve_child_block -- Resolve a child OID to a physical block address.
 *
 * For physical trees (omap_root == 0), the OID is the physical address
 * directly. For virtual trees, resolve through the object map.
 */
static int resolve_child_block(apfs_volume_t* vol,
                               uint64_t omap_root, uint64_t max_xid,
                               uint64_t child_oid, uint64_t* out_paddr) {
	if (omap_root == 0) {
		/* Physical tree: OID is the block address */
		*out_paddr = child_oid;
		return 0;
	}
	return apfs_omap_lookup(vol, omap_root, child_oid, max_xid, out_paddr);
}

/*
 * node_entry_oid -- Extract the APFS object id from the key of TOC entry i.
 *
 * Every FS-tree key begins with a j_key_t obj_id_and_type word (object id
 * in the low 60 bits). Used to prune enumerate-mode descent: the tree is
 * sorted by object id ascending, so we only descend into children whose
 * key range can contain the target object id.
 */
static uint64_t node_entry_oid(const uint8_t* buf, uint32_t toc_start,
                               uint32_t keys_start, int fixed_kv,
                               uint32_t i) {
	const uint8_t* key_ptr;
	uint64_t w;

	if (fixed_kv) {
		const kvoff_t* entry = (const kvoff_t*)(buf + toc_start + i * sizeof(kvoff_t));
		key_ptr = buf + keys_start + entry->k;
	} else {
		const kvloc_t* entry = (const kvloc_t*)(buf + toc_start + i * sizeof(kvloc_t));
		key_ptr = buf + keys_start + entry->k.off;
	}
	memcpy(&w, key_ptr, sizeof(uint64_t));
	return w & OBJ_ID_MASK;
}

/* ========================================================================== */
/* Core recursive traversal                                                   */
/* ========================================================================== */

/*
 * btree_node_traverse -- Recursive B-tree node traversal.
 *
 * block_addr:     physical block address of the current node
 * search_key:     key to search for (NULL for enumerate-all mode)
 * search_key_len: length of the search key
 * visitor:        callback for matching leaf records
 * user_data:      opaque context for the visitor
 * omap_root:      omap root for virtual child resolution (0 = physical)
 * max_xid:        max XID for omap lookups
 * parent_flags:   bt_flags from btree_info_t (propagated from root)
 * fixed_key_size: key size for fixed-KV trees (from btree_info_t)
 * fixed_val_size: value size for fixed-KV trees (from btree_info_t)
 * enum_oid:	   OID for the lookup (used to restrict searches)
 */
static int btree_node_traverse(apfs_volume_t* vol,
                               uint64_t block_addr,
                               const void* search_key,
                               uint16_t search_key_len,
                               btree_visitor_fn visitor,
                               void* user_data,
                               uint64_t omap_root,
                               uint64_t max_xid,
                               uint32_t parent_flags,
                               uint32_t fixed_key_size,
                               uint32_t fixed_val_size,
                               uint64_t enum_oid) {
	uint32_t bsize;
	void* buf;
	btree_node_phys_t* node;
	uint16_t flags;
	uint32_t nkeys;
	uint32_t bt_flags;
	uint32_t vals_end_offset;
	uint32_t fk_size;
	uint32_t fv_size;
	btree_info_t* info;
	int fixed_kv;
	uint32_t toc_start;
	uint32_t keys_start;
	int rc;
	uint32_t i;

	bsize = vol->block_size;
	buf = (void*)_HeapAllocate(bsize, 0);
	if (buf == NULL) {
		Debug_Printf("APFS: btree: out of memory in traversal\r\n");
		return -1;
	}

	if (apfs_read_block(vol, block_addr, buf) != 0) {
		Debug_Printf("APFS: btree: cannot read node at block 0x%08lX%08lX\r\n",
		             (DWORD)(block_addr >> 32),
		             (DWORD)(block_addr & 0xFFFFFFFF));
		_HeapFree(buf, 0);
		return -1;
	}

	if (apfs_verify_block(buf, bsize) != 0) {
		Debug_Printf("APFS: btree: checksum invalid at block 0x%08lX%08lX\r\n",
		             (DWORD)(block_addr >> 32),
		             (DWORD)(block_addr & 0xFFFFFFFF));
		_HeapFree(buf, 0);
		return -1;
	}

	node = (btree_node_phys_t*)buf;
	flags = node->btn_flags;
	nkeys = node->btn_nkeys;
	bt_flags = parent_flags;
	vals_end_offset = bsize;
	fk_size = fixed_key_size;
	fv_size = fixed_val_size;

	if (flags & BTNODE_ROOT) {
		/* Root node has btree_info_t at the end of the block */
		info = (btree_info_t*)((uint8_t*)buf + bsize - sizeof(btree_info_t));
		bt_flags = info->bt_fixed.bt_flags;
		fk_size = info->bt_fixed.bt_key_size;
		fv_size = info->bt_fixed.bt_val_size;
		vals_end_offset = bsize - sizeof(btree_info_t);
	}

	fixed_kv = (flags & BTNODE_FIXED_KV_SIZE) ? 1 : 0;
	toc_start = sizeof(btree_node_phys_t) + node->btn_table_space.off;
	keys_start = sizeof(btree_node_phys_t) + node->btn_table_space.off + node->btn_table_space.len;
	rc = 0;

	if (flags & BTNODE_LEAF) {
		/* ---- Leaf node: iterate records and call visitor ---- */
		for (i = 0; i < nkeys; i++) {
			const uint8_t* key_ptr;
			const uint8_t* val_ptr;
			uint16_t key_len;
			uint16_t val_len;
			uint16_t cmp_len;
			int vrc;

			if (fixed_kv) {
				kvoff_t* entry = (kvoff_t*)((uint8_t*)buf + toc_start + i * sizeof(kvoff_t));
				key_ptr = (uint8_t*)buf + keys_start + entry->k;
				val_ptr = (uint8_t*)buf + vals_end_offset - entry->v;
				key_len = (uint16_t)fk_size;
				val_len = (uint16_t)fv_size;
			} else {
				kvloc_t* entry = (kvloc_t*)((uint8_t*)buf + toc_start + i * sizeof(kvloc_t));
				key_ptr = (uint8_t*)buf + keys_start + entry->k.off;
				key_len = entry->k.len;
				val_ptr = (uint8_t*)buf + vals_end_offset - entry->v.off;
				val_len = entry->v.len;
			}

			/*
			 * If a search key is provided, use prefix matching:
			 * compare only up to search_key_len bytes. Note 
			 * the comparison isn't supposed to be an order check
			 * just an equality check.
			 */
			if (search_key != NULL) {
				cmp_len = key_len < search_key_len ? key_len : search_key_len;
				if (memcmp(key_ptr, search_key, cmp_len) != 0) continue;
			}

			vrc = visitor(key_ptr, key_len, val_ptr, val_len, user_data);
			if (vrc != 0) break; 
		}
	} else {
		/*
		 * ---- Internal node: find the right child to descend ----
		 *
		 * For internal nodes, each TOC entry maps a key to a child
		 * OID. In search mode we find the last entry whose key is
		 * <= search_key and descend into it. In enumerate mode
		 * (search_key == NULL) we descend into the children whose
		 * key range can contain enum_oid (see below).
		 */
		if (search_key == NULL) {
			/*
			 * Enumerate mode: descend only into children whose key
			 * range can contain records for enum_oid.
			 *
			 * The FS tree is sorted by object id ascending. Child i
			 * covers keys in [node[i],node[i+1]). It can hold the
			 * target oid only if node[i].oid <= enum_oid AND the next
			 * node (if any) has oid >= enum_oid. Nodes are
			 * ascending, so once node[i].oid > enum_oid no later child
			 * can match and we stop. This turns a whole-tree walk
			 * into O(height + matching leaves) -- without it, every
			 * inode/dir/extent lookup rescans the entire B-tree.
			 */
			for (i = 0; i < nkeys; i++) {
				uint64_t child_oid;
				uint64_t child_paddr;
				uint64_t node_oid;
				const uint8_t* val_p;

				node_oid = node_entry_oid((uint8_t*)buf, toc_start, keys_start, fixed_kv, i);
				if (node_oid > enum_oid) break;  /* this and all later children are above target */
				if (i + 1 < nkeys) {
					uint64_t next_oid = node_entry_oid((uint8_t*)buf, toc_start, keys_start, fixed_kv, i + 1);
					if (next_oid < enum_oid) continue;  /* child i entirely below target */
				}

				if (fixed_kv) {
					kvoff_t* entry;
					entry = (kvoff_t*)((uint8_t*)buf + toc_start + i * sizeof(kvoff_t));
					val_p = (uint8_t*)buf + vals_end_offset - entry->v;
				} else {
					kvloc_t* entry;
					entry = (kvloc_t*)((uint8_t*)buf + toc_start + i * sizeof(kvloc_t));
					val_p = (uint8_t*)buf + vals_end_offset - entry->v.off;
				}

				memcpy(&child_oid, val_p, sizeof(uint64_t));
				if (resolve_child_block(vol, omap_root,
				    max_xid, child_oid,
				    &child_paddr) != 0) {
					Debug_Printf("APFS: btree: cannot resolve child OID 0x%08lX%08lX\r\n",
					    (DWORD)(child_oid >> 32),
					    (DWORD)(child_oid & 0xFFFFFFFF));
					rc = -1;
					break;
				}

				rc = btree_node_traverse(vol, child_paddr,
				         search_key, search_key_len,
				         visitor, user_data,
				         omap_root, max_xid,
				         bt_flags, fk_size, fv_size, enum_oid);
				if (rc != 0) break;
			}
		} else {
			/* Search mode: find the correct child */
			uint32_t target_idx;
			uint64_t child_oid;
			uint64_t child_paddr;
			const uint8_t* val_p;

			target_idx = 0;

			for (i = 0; i < nkeys; i++) {
				const uint8_t* key_ptr;
				uint16_t key_len;
				int cmp;

				if (fixed_kv) {
					kvoff_t* entry;
					entry = (kvoff_t*)((uint8_t*)buf + toc_start + i * sizeof(kvoff_t));
					key_ptr = (uint8_t*)buf + keys_start + entry->k;
					key_len = (uint16_t)fk_size;
				} else {
					kvloc_t* entry;
					entry = (kvloc_t*)((uint8_t*)buf + toc_start + i * sizeof(kvloc_t));
					key_ptr = (uint8_t*)buf + keys_start + entry->k.off;
					key_len = entry->k.len;
				}

				cmp = omap_key_compare(key_ptr, key_len, search_key, search_key_len);
				if (cmp <= 0) target_idx = i;
				else break;
			}

			/* Read the child OID from the target entry */
			if (fixed_kv) {
				kvoff_t* entry;
				entry = (kvoff_t*)((uint8_t*)buf + toc_start + target_idx * sizeof(kvoff_t));
				val_p = (uint8_t*)buf + vals_end_offset - entry->v;
			} else {
				kvloc_t* entry;
				entry = (kvloc_t*)((uint8_t*)buf + toc_start + target_idx * sizeof(kvloc_t));
				val_p = (uint8_t*)buf + vals_end_offset - entry->v.off;
			}

			memcpy(&child_oid, val_p, sizeof(uint64_t));
			if (resolve_child_block(vol, omap_root, max_xid,
			    child_oid, &child_paddr) != 0) {
				Debug_Printf("APFS: btree: cannot resolve child OID 0x%08lX%08lX\r\n",
				    (DWORD)(child_oid >> 32),
				    (DWORD)(child_oid & 0xFFFFFFFF));
				_HeapFree(buf, 0);
				return -1;
			}

			rc = btree_node_traverse(vol, child_paddr,
			         search_key, search_key_len,
			         visitor, user_data,
			         omap_root, max_xid,
			         bt_flags, fk_size, fv_size, enum_oid);
		}
	}

	_HeapFree(buf, 0);
	return rc;
}

/* ========================================================================== */
/* Enumeration filter context and visitor                                     */
/* ========================================================================== */

/* Context for apfs_btree_enumerate's filtering visitor */
typedef struct {
	btree_visitor_fn real_visitor;
	void*            real_user_data;
	uint64_t         target_oid;
	uint8_t          target_type;
} enum_ctx_t;

/*
 * enum_filter_visitor -- Wrapper visitor that filters leaf records by
 * OID and record type. Only forwards matching records to the real
 * visitor.
 */
static int enum_filter_visitor(const void* key, uint16_t key_len,
                               const void* val, uint16_t val_len,
                               void* ctx_ptr) {
	enum_ctx_t* ctx;
	const j_key_t* jk;
	uint64_t k_oid;
	uint8_t k_type;

	ctx = (enum_ctx_t*)ctx_ptr;

	if (key_len < sizeof(j_key_t)) return 0; /* Skip malformed or corrupted entry */

	jk = (const j_key_t*)key;
	k_oid = jk->obj_id_and_type & OBJ_ID_MASK;
	k_type = (uint8_t)((jk->obj_id_and_type & OBJ_TYPE_MASK) >> OBJ_TYPE_SHIFT);

	if (k_oid == ctx->target_oid && k_type == ctx->target_type) {
		return ctx->real_visitor(key, key_len, val, val_len, ctx->real_user_data);
	}
	return 0;  /* continue */
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_btree_lookup -- Search a B-tree for records matching a key.
 *
 * This is the standard entry point for FS tree lookups. It uses the
 * volume's omap root and max_xid for virtual child resolution.
 *
 * Returns 0 on success, -1 on error.
 */
int apfs_btree_lookup(apfs_volume_t* vol, uint64_t root_block,
                      const void* search_key, uint16_t key_len,
                      btree_visitor_fn visitor, void* user_data) {
	if (vol == NULL || search_key == NULL || visitor == NULL) return -1;
	return btree_node_traverse(vol, root_block, search_key, key_len,
	                           visitor, user_data,
	                           vol->volume_omap_root, vol->max_xid,
	                           0, 0, 0, 0);
}

/*
 * apfs_btree_lookup_physical -- Search a physical B-tree (no omap
 * resolution for child pointers).
 *
 * Used by apfs_omap_lookup() to search the omap B-tree itself, which
 * is a physical tree where child OIDs are direct block addresses.
 *
 * Returns 0 on success, -1 on error.
 */
int apfs_btree_lookup_physical(apfs_volume_t* vol, uint64_t root_block,
                               const void* search_key, uint16_t key_len,
                               btree_visitor_fn visitor, void* user_data) {
	if (vol == NULL || search_key == NULL || visitor == NULL) return -1;
	return btree_node_traverse(vol, root_block, search_key, key_len,
	                           visitor, user_data,
	                           0, 0,
	                           0, 0, 0, 0);
}

/*
 * apfs_btree_enumerate -- Enumerate all B-tree records matching an OID
 * and record type.
 *
 * Filters records by OID and type using the enum_filter_visitor
 * wrapper. Uses the volume's omap root and max_xid for virtual child
 * resolution.
 *
 * We pass NULL as search_key (enumerate mode) rather than a single
 * prefix descent, because one object id may own several record types
 * (inode, dir recs, extents, xattrs) spanning more than one leaf. The
 * traversal is still bounded: the target oid is passed as enum_oid so
 * the descent prunes to only the subtrees that can contain that oid,
 * which relies on the FS tree being sorted by object id ascending.
 *
 * Returns 0 on success, -1 on error.
 */
int apfs_btree_enumerate(apfs_volume_t* vol, uint64_t root_block,
                         uint64_t oid, uint8_t record_type,
                         btree_visitor_fn visitor, void* user_data) {
	enum_ctx_t ctx;
	if (vol == NULL || visitor == NULL) return -1;

	ctx.real_visitor = visitor;
	ctx.real_user_data = user_data;
	ctx.target_oid = oid;
	ctx.target_type = record_type;

	return btree_node_traverse(vol, root_block, NULL, 0,
	                           enum_filter_visitor, &ctx,
	                           vol->volume_omap_root, vol->max_xid,
	                           0, 0, 0, oid);
}
