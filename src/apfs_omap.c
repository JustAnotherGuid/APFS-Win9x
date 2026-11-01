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
 * apfs_omap.c -- APFS object map B-tree lookup
 *
 * Resolves virtual object identifiers (OIDs) to physical block
 * addresses by searching the object map B-tree for the entry with
 * matching OID and highest XID <= max_xid.
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Static helpers                                                             */
/* ========================================================================== */

/*
 * omap_lookup_ctx_t -- Context passed to the B-tree visitor callback.
 *
 * Tracks the best (highest XID) omap entry found so far that matches
 * the target OID and has XID <= max_xid.
 */
typedef struct {
	uint64_t target_oid;
	uint64_t max_xid;
	uint64_t best_xid;
	uint64_t best_paddr;
	int      found;
} omap_lookup_ctx_t;

/*
 * omap_visitor -- B-tree visitor callback for omap lookup.
 *
 * Called for each leaf record whose key prefix matches our search key
 * (i.e., matching OID). We select the entry with matching OID and
 * highest XID <= max_xid.
 *
 * Returns 0 to continue scanning for a potentially higher XID.
 */
static int omap_visitor(const void* key, uint16_t key_len,
                        const void* val, uint16_t val_len,
                        void* user_data) {
	omap_lookup_ctx_t* ctx;
	const omap_key_t* ok;
	const omap_val_t* ov;

	ctx = (omap_lookup_ctx_t*)user_data;

	if (key_len < sizeof(omap_key_t) || val_len < sizeof(omap_val_t)) return 0;

	ok = (const omap_key_t*)key;
	ov = (const omap_val_t*)val;

	if (ok->ok_oid != ctx->target_oid) return 0;
	if (ok->ok_xid > ctx->max_xid) return 0;

	/* Keep the entry with the highest valid XID */
	if (!ctx->found || ok->ok_xid > ctx->best_xid) {
		ctx->best_xid = ok->ok_xid;
		ctx->best_paddr = ov->ov_paddr;
		ctx->found = 1;
	}

	return 0;  /* continue scanning for a higher XID */
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_omap_lookup -- Resolve a virtual OID to a physical block address.
 *
 * Searches the object map B-tree (rooted at omap_root) for the entry
 * with matching OID and highest XID <= max_xid, then returns the
 * physical block address (ov_paddr) of that entry.
 *
 * The search key uses sizeof(uint64_t) (8 bytes) as the key length,
 * NOT sizeof(omap_key_t) (16 bytes). This causes the B-tree lookup's
 * prefix comparison to match all entries sharing the target OID
 * regardless of their XID value. The visitor callback then filters
 * by max_xid to select the best entry.
 *
 * The omap B-tree is physical -- child OIDs are direct block addresses,
 * so we use apfs_btree_lookup_physical() which does not attempt omap
 * resolution for child pointers.
 *
 * Returns 0 on success, -1 if not found or on error.
 */
int apfs_omap_lookup(apfs_volume_t* vol, uint64_t omap_root,
                     uint64_t oid, uint64_t max_xid,
                     uint64_t* out_paddr) {
	omap_lookup_ctx_t ctx;
	omap_key_t search_key;
	int rc;

	if (vol == NULL || out_paddr == NULL) {
		return -1;
	}

	/* Initialize lookup context */
	memset(&ctx, 0, sizeof(ctx));
	ctx.target_oid = oid;
	ctx.max_xid = max_xid;

	/*
	 * Build search key: ok_oid = target OID, ok_xid = max_xid.
	 *
	 * We pass sizeof(uint64_t) as the search key length so the
	 * B-tree's prefix comparison matches all entries sharing this
	 * OID regardless of XID. The visitor then filters by max_xid
	 * to select the best (highest valid XID) entry.
	 */
	search_key.ok_oid = oid;
	search_key.ok_xid = max_xid;

	/*
	 * The omap B-tree is physical -- child OIDs are direct block
	 * addresses. Use apfs_btree_lookup_physical() which passes
	 * omap_root=0 and max_xid=0 internally, meaning no omap
	 * resolution is attempted for child pointers.
	 */
	rc = apfs_btree_lookup_physical(vol, omap_root, &search_key, sizeof(uint64_t), omap_visitor, &ctx);
	if (rc != 0) {
		Debug_Printf("APFS: omap: B-tree lookup failed for OID "
		             "0x%08lX%08lX\r\n",
		             (DWORD)(oid >> 32),
		             (DWORD)(oid & 0xFFFFFFFF));
		return -1;
	}

	if (!ctx.found) {
		Debug_Printf("APFS: omap: no entry for OID 0x%08lX%08lX "
		             "(max XID 0x%08lX%08lX)\r\n",
		             (DWORD)(oid >> 32),
		             (DWORD)(oid & 0xFFFFFFFF),
		             (DWORD)(max_xid >> 32),
		             (DWORD)(max_xid & 0xFFFFFFFF));
		return -1;
	}

	*out_paddr = ctx.best_paddr;
	return 0;
}
