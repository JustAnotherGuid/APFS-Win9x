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
 * apfs_dispatch.c -- Volume-level IFS function dispatch
 *
 * Implements the volfunc table entries for directory operations,
 * file attributes, open, find, disk info, and read-only stubs.
 *
 */

#include "apfs_vxd.h"

/*
 * apfs_open_misc -- Handle-level misc functions for open file handles.
 *
 * Seek and close are implemented in apfs_handle.c.
 * Commit, filelocks, piperequest, handleinfo are no-ops (read-only).
 * Filetimes and enumhandle are implemented in apfs_handle.c.
 */
hndlmisc apfs_open_misc = {
	IFS_VERSION,                            /* hm_version */
	IFS_REVISION,                           /* hm_revision */
	NUM_HNDLMISC,                           /* hm_size */
	{
		(pIFSFunc) apfs_file_seek,              /* HM_SEEK         = 0 */
		(pIFSFunc) apfs_close_file,             /* HM_CLOSE        = 1 */
		(pIFSFunc) apfs_empty_func,             /* HM_COMMIT       = 2 */
		(pIFSFunc) apfs_empty_func,             /* HM_FILELOCKS    = 3 */
		(pIFSFunc) apfs_file_datetime,          /* HM_FILETIMES    = 4 */
		(pIFSFunc) apfs_empty_func,             /* HM_PIPEREQUEST  = 5 */
		(pIFSFunc) apfs_empty_func,             /* HM_HANDLEINFO   = 6 */
		(pIFSFunc) apfs_enumerate_handle        /* HM_ENUMHANDLE   = 7 */
	}
};

/*
 * apfs_find_misc -- Handle-level misc functions for find handles.
 *
 * Only close is meaningful (frees the find context).
 * All other operations are no-ops.
 */
hndlmisc apfs_find_misc = {
	IFS_VERSION,                            /* hm_version */
	IFS_REVISION,                           /* hm_revision */
	NUM_HNDLMISC,                           /* hm_size */
	{
		(pIFSFunc) apfs_empty_func,           /* HM_SEEK         = 0 */
		(pIFSFunc) apfs_find_close,           /* HM_CLOSE        = 1 */
		(pIFSFunc) apfs_empty_func,           /* HM_COMMIT       = 2 */
		(pIFSFunc) apfs_empty_func,           /* HM_FILELOCKS    = 3 */
		(pIFSFunc) apfs_empty_func,           /* HM_FILETIMES    = 4 */
		(pIFSFunc) apfs_empty_func,           /* HM_PIPEREQUEST  = 5 */
		(pIFSFunc) apfs_empty_func,           /* HM_HANDLEINFO   = 6 */
		(pIFSFunc) apfs_empty_func            /* HM_ENUMHANDLE   = 7 */
	}
};

/*
 * attrib_resolve_ctx_t -- Helper context for resolving a path and
 * looking up the inode in one step.
 */
typedef struct {
	apfs_volume_t*  vol;
	uint64_t        ino;
	uint8_t         dtype;
	j_inode_val_t   inode;
	j_dstream_t     dstream;
	int             has_dstream;
} attrib_resolve_ctx_t;

/*
 * attrib_resolve -- Resolve path, look up inode, extract dstream.
 * Returns 0 on success, Win32 error code on failure.
 */
static int attrib_resolve(attrib_resolve_ctx_t* ctx, apfs_volume_t* vol,
                          ParsedPath* path) {
	int rc;

	ctx->vol = vol;
	ctx->has_dstream = 0;

	rc = apfs_resolve_path(vol, path, &ctx->ino, &ctx->dtype);
	if (rc != 0) return rc;

	rc = apfs_lookup_inode(vol, ctx->ino, &ctx->inode);
	if (rc != 0) return ERROR_FILE_NOT_FOUND;

	return 0;
}

/*
 * find_count_ctx_t -- First-pass visitor context: counts DIR_REC entries.
 */
typedef struct {
	uint32_t count;
} find_count_ctx_t;

static int find_count_visitor(const void* key, uint16_t key_len,
                              const void* val, uint16_t val_len,
                              void* user_data) {
	find_count_ctx_t* ctx;

	ctx = (find_count_ctx_t*)user_data;
	if (key_len >= sizeof(j_drec_key_t) && val_len >= sizeof(j_drec_val_t)) {
		ctx->count++;
	}
	return 0;  /* continue */
}

/*
 * find_populate_ctx_t -- Second-pass visitor context: Populates the
 * dir_entry_cache array with directory entry metadata.
 */
typedef struct {
	apfs_volume_t*      vol;
	dir_entry_cache_t*  entries;
	uint32_t            capacity;
	uint32_t            index;
} find_populate_ctx_t;

static int find_populate_visitor(const void* key, uint16_t key_len,
                                 const void* val, uint16_t val_len,
                                 void* user_data) {
	find_populate_ctx_t* ctx;
	const j_drec_key_t* drec_key;
	const j_drec_val_t* drec_val;
	const uint8_t* name_bytes;
	uint16_t name_len;
	dir_entry_cache_t* entry;
	int uni_len;
	j_inode_val_t inode;
	uint64_t entry_size;
	uint8_t dtype;

	ctx = (find_populate_ctx_t*)user_data;

	if (ctx->index >= ctx->capacity) return 1;
	if (key_len < sizeof(j_drec_key_t) || val_len < sizeof(j_drec_val_t)) return 0;

	drec_key = (const j_drec_key_t*)key;
	drec_val = (const j_drec_val_t*)val;

	/* Extract name from directory record key */
	name_len = (uint16_t)(drec_key->name_len_and_hash & J_DREC_LEN_MASK);
	if (name_len <= 1) return 0;
	name_len = name_len - 1;  /* exclude null terminator */

	name_bytes = (const uint8_t*)key + sizeof(j_drec_key_t);
	if ((uint16_t)(sizeof(j_drec_key_t) + name_len) > key_len) return 0;

	entry = &ctx->entries[ctx->index];
	memset(entry, 0, sizeof(dir_entry_cache_t));

	uni_len = apfs_name_to_unicode(name_bytes, name_len, entry->name, 255);
	if (uni_len <= 0) return 0;
	entry->name[uni_len] = 0;  /* null-terminate */
	entry->name_len = (uint16_t)uni_len;

	/* Store basic info from directory record */
	entry->ino = drec_val->file_id;
	dtype = (uint8_t)(drec_val->flags & DREC_TYPE_MASK);
	entry->d_type = dtype;

	/* Look up the target inode for metadata. Use the dstream-aware
	 * helper so files report their real size (the fixed inode's
	 * uncompressed_size is 0 for normal APFS files). */
	if (apfs_inode_info(ctx->vol, entry->ino, &inode, &entry_size) == 0) {
		entry->mode = inode.mode;
		entry->size = (dtype == DT_DIR) ? 0 : entry_size;

		entry->created  = apfs_to_dos_time(inode.create_time);
		entry->modified = apfs_to_dos_time(inode.mod_time);
		entry->accessed = apfs_to_dos_time(inode.access_time);
	} else {
		entry->mode = 0;
		entry->size = 0;
		entry->created  = apfs_to_dos_time(0);
		entry->modified = apfs_to_dos_time(0);
		entry->accessed = apfs_to_dos_time(0);
	}


	entry->attr = apfs_synthesize_dos_attr(dtype, entry->mode, entry->name, entry->name_len);
	apfs_generate_short_name(entry->name, entry->name_len, entry->short_name);

	ctx->index++;
	return 0;
}


/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */


/*
 * apfs_delete_file -- Reject file deletion (read-only volume).
 */
int _cdecl apfs_delete_file(pioreq pir) {
	Debug_Printf("APFS: entering apfs_delete_file\r\n");
	pir->ir_error = ERROR_ACCESS_DENIED;
	return pir->ir_error;
}

/*
 * apfs_rename_file -- Reject file rename (read-only volume).
 */
int _cdecl apfs_rename_file(pioreq pir) {
	Debug_Printf("APFS: entering apfs_rename_file\r\n");
	pir->ir_error = ERROR_ACCESS_DENIED;
	return pir->ir_error;
}

/*
 * ApfsFlushVolume -- No-op for read-only volume.
 */
int _cdecl apfs_flush_volume(pioreq pir) {
	Debug_Printf("APFS: entering apfs_flush_volume\r\n");
	return 0;
}

/*
 * apfs_empty_func -- Generic no-op stub for unused function slots.
 */
int _cdecl apfs_empty_func(pioreq pir) {
	Debug_Printf("APFS: entering ApfsEmptyFunc\r\n");
	return 0;
}


int _cdecl apfs_dir(pioreq pir) {
	apfs_volume_t* vol;
	int subfunc;
	uint64_t ino;
	uint8_t dtype;
	int rc;

	Debug_Printf("APFS: entering apfs_dir\r\n");

	vol = (apfs_volume_t*) pir->ir_rh;
	subfunc = pir->ir_flags;

	switch (subfunc) {

	case CREATE_DIR:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case DELETE_DIR:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case CHECK_DIR:
		/*
		 * Verify that the given path refers to an existing directory.
		 * Root path is always valid. Otherwise resolve and check DT_DIR.
		 */
		if (IFSIsRoot(pir->ir_ppath)) {
			return 0;
		}

		rc = apfs_resolve_path(vol, pir->ir_ppath, &ino, &dtype);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}

		if (dtype != DT_DIR) {
			pir->ir_error = ERROR_PATH_NOT_FOUND;
			return ERROR_PATH_NOT_FOUND;
		}

		return 0;

	case QUERY83_DIR:
		/*
		 * For now, we simply verify the path exists and copy
		 * the original path to ppath2 (the 8.3 generation is
		 * is broken atm because we are just adding 1). To do this
		 * properly for each folder would have to check its siblings
		 * and generate the names based on the order they appear.
		 */
		rc = apfs_resolve_path(vol, pir->ir_ppath, &ino, &dtype);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}

		/* Copy the original parsed path to ppath2 as a fallback.
		 * A full implementation would walk each component and
		 * generate 8.3 names, but the path structure is opaque
		 * enough that copying is the safe approach for now. */
		if (pir->ir_ppath2 != NULL) {
			memcpy(pir->ir_ppath2, pir->ir_ppath,
			       pir->ir_ppath->pp_totalLength
			       + sizeof(pir->ir_ppath->pp_totalLength));
		}
		return 0;

	case QUERYLONG_DIR:
		/*
		 * Build the long-name version of the path. APFS already
		 * stores long names, so we verify the path exists and
		 * copy the original path to ppath2.
		 */
		rc = apfs_resolve_path(vol, pir->ir_ppath, &ino, &dtype);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}

		if (pir->ir_ppath2 != NULL) {
			memcpy(pir->ir_ppath2, pir->ir_ppath,
			       pir->ir_ppath->pp_totalLength
			       + sizeof(pir->ir_ppath->pp_totalLength));
		}
		return 0;

	default:
		pir->ir_error = ERROR_INVALID_FUNCTION;
		return ERROR_INVALID_FUNCTION;
	}
}


int _cdecl apfs_file_attributes(pioreq pir) {
	apfs_volume_t* vol;
	int subfunc;
	attrib_resolve_ctx_t ctx;
	int rc;
	BYTE attr;

	Debug_Printf("APFS: entering apfs_file_attributes\r\n");

	vol = (apfs_volume_t*) pir->ir_rh;
	subfunc = pir->ir_flags;

	switch (subfunc) {

	case GET_ATTRIBUTES:
		/*
		 * Resolve path, synthesize DOS attributes from APFS metadata.
		 */
		rc = attrib_resolve(&ctx, vol, pir->ir_ppath);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}
		
		attr = apfs_synthesize_dos_attr(ctx.dtype, ctx.inode.mode, NULL, 0);
		pir->ir_attr = attr;
		return 0;

	case SET_ATTRIBUTES:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case GET_ATTRIB_COMP_FILESIZE:
		/*
		 * Return the compressed (actual) file size.
		 * For APFS, this is the dstream size.
		 */
		rc = attrib_resolve(&ctx, vol, pir->ir_ppath);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}

		/* For directories, size is 0 */
		if (ctx.dtype == DT_DIR) {
			pir->ir_size = 0;
			return 0;
		}

		/* Get the real file size from the DSTREAM xfield. The fixed
		 * inode's uncompressed_size is 0 for normal APFS files. */
		{
			uint64_t fsize = 0;
			if (apfs_inode_info(vol, ctx.ino, NULL, &fsize) == 0) {
				pir->ir_size = (fsize > 0xFFFFFFFF) ? 0xFFFFFFFF : (ULONG) fsize;
			} else {
				pir->ir_size = (ULONG) ctx.inode.uncompressed_size;
			}
		}
		return 0;

	case GET_ATTRIB_MODIFY_DATETIME:
		rc = attrib_resolve(&ctx, vol, pir->ir_ppath);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}
		pir->ir_dostime = apfs_to_dos_time(ctx.inode.mod_time);
		return 0;

	case SET_ATTRIB_MODIFY_DATETIME:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case GET_ATTRIB_CREATE_DATETIME:
		rc = attrib_resolve(&ctx, vol, pir->ir_ppath);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}
		pir->ir_dostime = apfs_to_dos_time(ctx.inode.create_time);
		return 0;

	case SET_ATTRIB_CREATE_DATETIME:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case GET_ATTRIB_ACCESS_DATETIME:
		rc = attrib_resolve(&ctx, vol, pir->ir_ppath);
		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}
		pir->ir_dostime = apfs_to_dos_time(ctx.inode.access_time);
		return 0;

	case SET_ATTRIB_ACCESS_DATETIME:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	default:
		pir->ir_error = ERROR_INVALID_FUNCTION;
		return ERROR_INVALID_FUNCTION;
	}
}


/*
 * apfs_get_disk_info -- Return volume size and free space information.
 *
 * For a read-only APFS volume:
 *   ir_length  = bytes per sector (block_size)
 *   ir_sectors = sectors per allocation unit (1)
 *   ir_size    = total allocation units (block_count)
 *   ir_numfree = free allocation units (0 -- read-only)
 */
int _cdecl apfs_get_disk_info(pioreq pir) {
	apfs_volume_t* vol;

	Debug_Printf("APFS: entering apfs_get_disk_info\r\n");

	vol = (apfs_volume_t*) pir->ir_rh;
	pir->ir_length  = vol->block_size;
	pir->ir_sectors = 1;
	pir->ir_size    = (ULONG) vol->container_sb.nx_block_count;
	pir->ir_numfree = 0;
	return 0;
}

/*
 * apfs_query_resource_info -- Return volume capabilities.
 *
 * Level 2 query returns:
 *   ir_length  = MAKELONG(max_component_length, max_path_length)
 *   ir_options = capability flags
 */
int _cdecl apfs_query_resource_info(pioreq pir) {
	Debug_Printf("APFS: entering apfs_query_resource_info\r\n");
	if (pir->ir_options == 2) {
		/* Level 2: volume capabilities */
		pir->ir_length = MAKELONG(255, MAX_PATH);
		pir->ir_options = FS_CASE_IS_PRESERVED
		                | FS_UNICODE_STORED_ON_DISK
		                | FS_VOL_SUPPORTS_LONG_NAMES;
		return 0;
	}

	pir->ir_error = ERROR_INVALID_FUNCTION;
	return ERROR_INVALID_FUNCTION;
}


/*
 * apfs_open_file -- Opens a file for reading. Rejects write access, resolves the path,  
 * looks up the inode, extracts the data stream, allocates an apfs_file_t
 * handle, and sets up the IFS response	
 */
int _cdecl apfs_open_file(pioreq pir) {
	apfs_volume_t* vol;
	int mode;
	uint64_t ino;
	uint8_t dtype;
	int rc;
	j_inode_val_t inode;
	uint64_t file_size;
	apfs_file_t* fp;
	BYTE attr;

	char path[MAX_NAME_CHARS];
	UniToBCSPath(path, pir->ir_ppath->pp_elements, MAX_NAME_CHARS, BCS_WANSI);
	Debug_Printf("APFS: OpenFile: Path=%s\r\n", path);
	Debug_Printf("APFS: OpenFile: Entering apfs_open_file\r\n");

	vol = (apfs_volume_t*) pir->ir_rh;

	mode = pir->ir_flags & ACCESS_MODE_MASK;
	if (mode == ACCESS_WRITEONLY || mode == ACCESS_READWRITE) {
		pir->ir_error = ERROR_WRITE_PROTECT;
		return pir->ir_error;
	}

	rc = apfs_resolve_path(vol, pir->ir_ppath, &ino, &dtype);
	if (rc != 0) {
		Debug_Printf("APFS: OpenFile: Failed to resolve path\n");
		pir->ir_error = rc;
		return rc;
	}

	/* Look up inode and its logical size (dstream-aware).
	 * apfs_inode_info() extracts the DSTREAM xfield rather than trusting
	 * the fixed inode's uncompressed_size, which is 0 for normal APFS
	 * files 
	 */
	file_size = 0;
	rc = apfs_inode_info(vol, ino, &inode, &file_size);
	if (rc != 0) {
		Debug_Printf("APFS: OpenFile: Failed to get inode info\n");
		pir->ir_error = ERROR_FILE_NOT_FOUND;
		return ERROR_FILE_NOT_FOUND;
	}

	Debug_Printf("APFS: OpenFile: dtype=%u mode=0x%04X size=%lu\r\n",
	             (DWORD)dtype, (DWORD)inode.mode, (DWORD)file_size);

	fp = (apfs_file_t*) _HeapAllocate(sizeof(apfs_file_t), HEAPZEROINIT);
	if (fp == NULL) {
		pir->ir_error = ERROR_NOT_ENOUGH_MEMORY;
		return pir->ir_error;
	}

	fp->vol = vol;
	fp->ino = ino;
	fp->private_id = inode.private_id;
	fp->size = file_size;
	fp->pos = 0;
	memcpy(&fp->inode, &inode, sizeof(j_inode_val_t));
	fp->d_type = dtype;
	fp->mode = inode.mode;

	pir->ir_fh = (fh_t) fp;

	/* ir_size is a MSVC "long" which is a 32-bit value.... */
	if (fp->size > 0xFFFFFFFF) {
		pir->ir_size = 0xFFFFFFFF;
	} else {
		pir->ir_size = (ULONG) fp->size;
	}

	pir->ir_dostime = apfs_to_dos_time(inode.mod_time);
	attr = apfs_synthesize_dos_attr(dtype, inode.mode, NULL, 0);
	pir->ir_attr = attr;

	SetHandleFunc(pir, (pIFSFunc) apfs_read_file, (pIFSFunc) apfs_write_file, &apfs_open_misc);
	Debug_Printf("APFS: OpenFile: ino=0x%08lX%08lX size=%lu\r\n",
	             (DWORD)(ino >> 32), (DWORD)(ino & 0xFFFFFFFF),
	             pir->ir_size);

	return 0;
}


int _cdecl apfs_find_first_file(pioreq pir) {
	apfs_volume_t* vol;
	_WIN32_FIND_DATA* fdp;
	apfs_find_t* find;
	uint64_t dir_ino;
	uint8_t dir_dtype;
	int rc;
	find_count_ctx_t count_ctx;
	find_populate_ctx_t pop_ctx;
	PathElement* pe;
	PathElement* end;
	PathElement* last_pe;
	PathElement* prev_pe;
	uint16_t pattern_len;
	uint32_t i;
	int match_result;

	pir->ir_error = 0;
	pir->ir_fh = 0;

	vol = (apfs_volume_t*) pir->ir_rh;
	fdp = (_WIN32_FIND_DATA*) pir->ir_data;

	Debug_Printf("APFS: FindFirst: vol=%lX fdp=%lX attr=0x%02X\r\n",
	             (DWORD)vol, (DWORD)fdp, (DWORD)pir->ir_attr);

	if (LOBYTE(pir->ir_attr) == FILE_ATTRIBUTE_LABEL) {
		uint16_t label_len;

		memset(fdp, 0, sizeof(_WIN32_FIND_DATA));
		fdp->dwFileAttributes = FILE_ATTRIBUTE_LABEL;

		for (label_len = 0; label_len < 255; label_len++) {
			if (vol->vol_label_uni[label_len] == 0) break;
		}
		if (label_len > 0) {
			memcpy(fdp->cFileName, vol->vol_label_uni, label_len * sizeof(USHORT));
		}

		Debug_Printf("APFS: FindFirst: Adding handle functions\r\n");
		SetHandleFunc(pir, (pIFSFunc) apfs_find_next_file, (pIFSFunc) apfs_empty_func, &apfs_find_misc);
		return 0;
	}

	Debug_Printf("APFS: FindFirst: resolving directory path\r\n");
	Debug_Printf("APFS: FindFirst: Searching for Root: %d\r\n", IFSIsRoot(pir->ir_ppath));

	/* 
	 * The IFSMgr convention is pp_elements[0] is the FIRST element, 
	 * and the elements run forward via IFSNextElement() up to the last one.
	 * We start at the first element and advance until the next step
	 * would leave the structure (or hit a zero-length terminator);
	 * that final element is the pattern (e.g. "*.*").
	 *
	 *   last_pe -> the search-pattern element
	 *   prev_pe -> the element just before it, or NULL when the path has
	 *              a single element (i.e. a search in the root dir, such
	 *              as "\*.*").
	 */
	pe = &pir->ir_ppath->pp_elements[0];
	end = (PathElement*)((char*)pir->ir_ppath + pir->ir_ppath->pp_totalLength + sizeof(pir->ir_ppath->pp_totalLength));

	last_pe = NULL;
	prev_pe = NULL;
	while ((char*)pe < (char*)end) {
		PathElement* next;

		/* Zero/short length marks the end (terminator element). */
		if (pe->pe_length <= sizeof(pe->pe_length)) break;

		next = IFSNextElement(pe);
		if ((char*)next >= (char*)end || next->pe_length <= sizeof(next->pe_length)) {
			/* pe is the final real element -- the search pattern. */
			last_pe = pe;
			break;
		}

		prev_pe = pe;
		pe = next;
	}

	if (last_pe == NULL) {
		pir->ir_error = ERROR_PATH_NOT_FOUND;
		return ERROR_PATH_NOT_FOUND;
	}


	/* If there's only one element, the directory is root. */
	if (prev_pe == NULL) {
		/* Only one path element -- directory is root */
		dir_ino = APFS_ROOT_DIR_INO;
		dir_dtype = DT_DIR;
	} else {
		/* Walk path up to (but not including) the last element.
		 * We temporarily shorten the path by adjusting totalLength,
		 * resolve, then restore. */
		uint16_t orig_total;
		uint16_t short_total;

		orig_total = pir->ir_ppath->pp_totalLength;
		short_total = (uint16_t)((char*)last_pe - (char*)pir->ir_ppath - sizeof(pir->ir_ppath->pp_totalLength));
		pir->ir_ppath->pp_totalLength = short_total;
		rc = apfs_resolve_path(vol, pir->ir_ppath, &dir_ino, &dir_dtype);
		pir->ir_ppath->pp_totalLength = orig_total;

		if (rc != 0) {
			pir->ir_error = rc;
			return rc;
		}

		if (dir_dtype != DT_DIR) {
			pir->ir_error = ERROR_PATH_NOT_FOUND;
			return ERROR_PATH_NOT_FOUND;
		}
	}

	if (last_pe == NULL || last_pe->pe_length <= sizeof(last_pe->pe_length)) {
		pir->ir_error = ERROR_FILE_NOT_FOUND;
		return ERROR_FILE_NOT_FOUND;
	}

	pattern_len = (uint16_t)((last_pe->pe_length - sizeof(last_pe->pe_length))/sizeof(USHORT));
	find = (apfs_find_t*) _HeapAllocate(sizeof(apfs_find_t), HEAPZEROINIT);
	if (find == NULL) {
		pir->ir_error = ERROR_NOT_ENOUGH_MEMORY;
		return pir->ir_error;
	}

	find->vol = vol;
	find->dir_ino = dir_ino;
	find->attr_mask = pir->ir_attr;

	if (pattern_len > 255) pattern_len = 255;
	memcpy(find->pattern, last_pe->pe_unichars, pattern_len * sizeof(USHORT));
	find->pattern[pattern_len] = 0;

	/* First pass get number of folders. */
	Debug_Printf("APFS: FindFirst: counting dir_ino=0x%08lX%08lX\r\n", (DWORD)(dir_ino >> 32), (DWORD)(dir_ino & 0xFFFFFFFF));
	count_ctx.count = 0;
	rc = apfs_list_dir(vol, dir_ino, find_count_visitor, &count_ctx);
	if (rc != 0 || count_ctx.count == 0) {
		Debug_Printf("APFS: FindFirst: count failed or empty (rc=%d count=%lu)\r\n", rc, (DWORD)count_ctx.count);
		_HeapFree(find, 0);
		pir->ir_error = ERROR_NO_MORE_FILES;
		return ERROR_NO_MORE_FILES;
	}

	Debug_Printf("APFS: FindFirst: %lu entries, allocating %lu bytes\r\n", (DWORD)count_ctx.count,
		(DWORD)(count_ctx.count * sizeof(dir_entry_cache_t)));
	find->entries = (dir_entry_cache_t*) _HeapAllocate(count_ctx.count * sizeof(dir_entry_cache_t), HEAPZEROINIT);
	if (find->entries == NULL) {
		Debug_Printf("APFS: FindFirst: alloc failed\r\n");
		_HeapFree(find, 0);
		pir->ir_error = ERROR_NOT_ENOUGH_MEMORY;
		return pir->ir_error;
	}

	/* Second pass populate entries */
	Debug_Printf("APFS: FindFirst: populating entries\r\n");
	pop_ctx.vol = vol;
	pop_ctx.entries = find->entries;
	pop_ctx.capacity = count_ctx.count;
	pop_ctx.index = 0;

	apfs_list_dir(vol, dir_ino, find_populate_visitor, &pop_ctx);
	find->entry_count = pop_ctx.index;
	Debug_Printf("APFS: FindFirst: populated %lu entries\r\n", (DWORD)find->entry_count);


	find->current_index = 0;
	for (i = 0; i < find->entry_count; i++) {
		dir_entry_cache_t* e;

		e = &find->entries[i];

		/* Apply pattern matching using IFSMgr_MetaMatch.
		 * IFSMgr_MetaMatch returns NON-ZERO on a match it 
		 * needs UFLG_META so '*'/'?' act as wildcards. */
		match_result = IFSMgr_MetaMatch((string_t) find->pattern, (string_t) e->name, UFLG_NT_DOS | UFLG_META);

		if (match_result != 0) {
			fill_find_data(fdp, e);
			find->current_index = i + 1;

			/* Set handle functions for FindNext/FindClose */
			pir->ir_fh = (fh_t) find;
			SetHandleFunc(pir, (pIFSFunc) apfs_find_next_file, (pIFSFunc) apfs_empty_func, &apfs_find_misc);
			Debug_Printf("APFS: FindFirst: matched '%s' (%lu entries)\r\n", fdp->cFileName, (DWORD) find->entry_count);
			return 0;
		}
	}

	/* No matching entry found */
	_HeapFree(find->entries, 0);
	_HeapFree(find, 0);

	pir->ir_error = ERROR_NO_MORE_FILES;
	return ERROR_NO_MORE_FILES;
}


/*
 * apfs_search_file -- DOS-style SEARCH_FIRST/SEARCH_NEXT.
 *
 * This is the legacy INT 21h FindFirst/FindNext interface. We return
 * ERROR_NO_MORE_FILES since Win32 applications use VFN_FINDOPEN instead.
 * DOS programs will see "File not found".
 */
int _cdecl apfs_search_file(pioreq pir) {
	Debug_Printf("APFS: entering apfs_search_file\r\n");
	pir->ir_error = ERROR_NO_MORE_FILES;
	return pir->ir_error;
}

/*
 * apfs_disconnect -- Unmount and clean up a volume.
 *
 * Called when the volume is being disconnected (e.g., USB removal,
 * explicit unmount). We free the volume context -- the actual
 * unmount is handled by the mount thunk's IR_FSD_UNLOAD case.
 */
int _cdecl apfs_disconnect(pioreq pir) {
	apfs_volume_t* vol;

	Debug_Printf("APFS: entering apfs_disconnect\r\n");

	vol = (apfs_volume_t*) pir->ir_rh;
	if (vol != NULL) {
		Debug_Printf("APFS: Disconnect on drive %c:\r\n",'A' + vol->drive);
	}

	/* The actual cleanup is done by IR_FSD_UNLOAD in the mount thunk.
	 * We just acknowledge the disconnect here. */
	return 0;
}

/*
 * apfs_ioctl16_drive -- Handle 16-bit IOCTL requests.
 *
 * We don't support any 16-bit IOCTLs. Return ERROR_INVALID_FUNCTION.
 */
int _cdecl apfs_ioctl16_drive(pioreq pir) {
	Debug_Printf("APFS: entering apfs_ioctl16_drive\r\n");
	pir->ir_error = ERROR_INVALID_FUNCTION;
	return pir->ir_error;
}

/*
 * apfs_direct_disk_io -- Handle direct disk I/O (DASD) requests.
 *
 * Direct disk access is not supported on our read-only APFS volume.
 * Return ERROR_ACCESS_DENIED.
 */
int _cdecl apfs_direct_disk_io(pioreq pir) {
	Debug_Printf("APFS: entering apfs_direct_disk_io\r\n");
	pir->ir_error = ERROR_ACCESS_DENIED;
	return pir->ir_error;
}

/*
 * apfs_get_disk_parms -- Return disk parameter block (DPB) information.
 *
 * This provides legacy DOS disk geometry information. For APFS volumes,
 * we return basic parameters derived from the container block size.
 */
int _cdecl apfs_get_disk_parms(pioreq pir) {
	apfs_volume_t* vol;

	Debug_Printf("APFS: entering ApfsGetDiskParms\r\n");

	vol = (apfs_volume_t*) pir->ir_rh;

	/*
	 * The DPB fields are set via ir_length (bytes per sector),
	 * ir_sectors (sectors per cluster), ir_size (total clusters).
	 * This mirrors what apfs_get_disk_info returns.
	 */
	pir->ir_length  = vol->block_size;
	pir->ir_sectors = 1;
	pir->ir_size    = (ULONG) vol->container_sb.nx_block_count;

	return 0;
}