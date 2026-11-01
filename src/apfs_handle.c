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
 * apfs_handle.c -- Handle-level IFS operations
 *
 * Implements per-handle functions: read, seek, close, datetime,
 * find-next, find-close, and write-protect stubs.
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
 * read_ctx_t -- Visitor context for extent-based file reading.
 *
 * Passed to the extent visitor callback. Tracks the current read
 * position, remaining bytes, output buffer pointer, and error state.
 */
typedef struct {
	apfs_volume_t*  vol;        /* volume context for block reads */
	uint64_t        pos;        /* current read position in file */
	uint32_t        remaining;  /* bytes still to read */
	uint8_t*        buf;        /* output buffer pointer (advances) */
	uint32_t        bytes_read; /* total bytes read so far */
	int             error;      /* error code if any */
} read_ctx_t;

/*
 * read_extent_visitor -- B-tree visitor callback for FILE_EXTENT records.
 *
 * For each extent that overlaps the read range [ctx->pos, ctx->pos +
 * ctx->remaining), reads the relevant blocks and copies data to the
 * user buffer.
 *
 * Returns 1 to stop enumeration when done (remaining == 0 or error),
 * 0 to continue to the next extent.
 */
static int read_extent_visitor(const void* key, uint16_t key_len,
                               const void* val, uint16_t val_len,
                               void* user_data) {
	read_ctx_t* ctx;
	const j_file_extent_key_t* ext_key;
	const j_file_extent_val_t* ext_val;
	uint64_t logical_addr;
	uint64_t phys_block;
	uint64_t extent_len;
	uint64_t ext_end;
	uint64_t read_end;
	uint64_t ext_offset;
	uint32_t ext_bytes;
	uint64_t start_block;
	uint32_t block_offset;
	uint32_t block_size;
	uint8_t* block_buf;
	uint32_t chunk;
	int rc;

	ctx = (read_ctx_t*)user_data;

	if (ctx->remaining == 0 || ctx->error != 0) return 1;
	if (key_len < sizeof(j_file_extent_key_t)) return 0;
	if (val_len < sizeof(j_file_extent_val_t)) return 0;

	ext_key = (const j_file_extent_key_t*)key;
	ext_val = (const j_file_extent_val_t*)val;

	logical_addr = ext_key->logical_addr;
	phys_block   = ext_val->phys_block_num;
	extent_len   = ext_val->len_and_flags & J_FILE_EXTENT_LEN_MASK;
	block_size   = ctx->vol->block_size;
	ext_end = logical_addr + extent_len;
	read_end = ctx->pos + (uint64_t)ctx->remaining;

	if (ext_end <= ctx->pos) return 0;
	if (logical_addr >= read_end) {
		/* Extent is entirely after read range -- we're done since
		 * extents are sorted by logical_addr */
		return 1;
	}

	if (ctx->pos > logical_addr) {
		ext_offset = ctx->pos - logical_addr;
	} else {
		ext_offset = 0;
	}

	ext_bytes = (uint32_t)(extent_len - ext_offset);
	if (ext_bytes > ctx->remaining) {
		ext_bytes = ctx->remaining;
	}
	start_block = phys_block + (ext_offset / (uint64_t)block_size);
	block_offset = (uint32_t)(ext_offset % (uint64_t)block_size);

	/* Allocate a temporary block buffer for partial reads */
	block_buf = (uint8_t*) _HeapAllocate(block_size, 0);
	if (block_buf == NULL) {
		ctx->error = ERROR_NOT_ENOUGH_MEMORY;
		return 1;
	}

	while (ext_bytes > 0) {
		chunk = block_size - block_offset;
		if (chunk > ext_bytes) {
			chunk = ext_bytes;
		}

		rc = apfs_read_block_without_cache(ctx->vol, start_block, block_buf);
		if (rc != 0) {
			Debug_Printf("APFS: ReadFile: block read failed "
			             "at block 0x%08lX%08lX\r\n",
			             (DWORD)(start_block >> 32),
			             (DWORD)(start_block & 0xFFFFFFFF));
			ctx->error = rc;
			_HeapFree(block_buf, 0);
			return 1;
		}

		/* Copy data from block to user buffer */
		memcpy(ctx->buf, block_buf + block_offset, chunk);

		ctx->buf += chunk;
		ctx->pos += (uint64_t)chunk;
		ctx->bytes_read += chunk;
		ctx->remaining -= chunk;
		ext_bytes -= chunk;

		/* After the first block, offset is always 0 */
		block_offset = 0;
		start_block++;
	}

	_HeapFree(block_buf, 0);
	return (ctx->remaining == 0) ? 1 : 0;
}

/*
 * apfs_read_file -- Read data from an open file handle.
 *
 * Extracts the apfs_file_t from pir->ir_fh, clamps the read to
 * file size, enumerates FILE_EXTENT records via apfs_list_extents(),
 * reads blocks, copies data to the user buffer, and updates the
 * file position.
 */
int _cdecl apfs_read_file(pioreq pir) {
	apfs_file_t* fp;
	apfs_volume_t* vol;
	uint64_t pos;
	uint32_t nbytes;
	read_ctx_t ctx;
	int rc;

	fp = (apfs_file_t*) pir->ir_fh;
	if (fp == NULL) {
		pir->ir_error = ERROR_INVALID_HANDLE;
		return pir->ir_error;
	}

	vol = fp->vol;
	pos = (uint64_t) pir->ir_pos;
	nbytes = pir->ir_length;

	if (pos >= fp->size) {
		pir->ir_length = 0;
		return 0;
	}

	if (pos + (uint64_t)nbytes > fp->size) {
		nbytes = (uint32_t)(fp->size - pos);
	}

	if (nbytes == 0) {
		pir->ir_length = 0;
		return 0;
	}

	ctx.vol = vol;
	ctx.pos = pos;
	ctx.remaining = nbytes;
	ctx.buf = (uint8_t*) pir->ir_data;
	ctx.bytes_read = 0;
	ctx.error = 0;

	rc = apfs_list_extents(vol, fp->private_id, read_extent_visitor, &ctx);

	if (ctx.error != 0) {
		pir->ir_error = ctx.error;
		pir->ir_length = ctx.bytes_read;
		return pir->ir_error;
	}

	if (rc != 0 && ctx.bytes_read == 0) {
		pir->ir_error = ERROR_READ_FAULT;
		pir->ir_length = 0;
		return pir->ir_error;
	}

	fp->pos = pos + (uint64_t)ctx.bytes_read;
	pir->ir_pos = (ULONG) fp->pos;
	pir->ir_length = ctx.bytes_read;

	return 0;
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

/*
 * apfs_write_file -- Return ERROR_WRITE_PROTECT for all write attempts.
 */
int _cdecl apfs_write_file(pioreq pir) {
	Debug_Printf("APFS: entering apfs_write_file\r\n");
	pir->ir_error = ERROR_WRITE_PROTECT;
	return pir->ir_error;
}

/*
 * apfs_file_seek -- Set the file position for an open file handle.
 *
 * pir->ir_pos contains the offset value.
 */
int _cdecl apfs_file_seek(pioreq pir) {
	apfs_file_t* fp;
	int method;
	uint64_t new_pos;
	int32_t offset;

	/* Avoid logging entry in apfs_file_seek because it is called very often. */

	fp = (apfs_file_t*) pir->ir_fh;
	if (fp == NULL) {
		pir->ir_error = ERROR_INVALID_HANDLE;
		return pir->ir_error;
	}

	method = pir->ir_flags;
	offset = (int32_t) pir->ir_pos;

	switch (method) {

	case FILE_BEGIN:
		new_pos = (uint64_t)(ULONG) pir->ir_pos;
		break;

	case FILE_CURRENT:
		new_pos = fp->pos + (int64_t)offset;
		break;

	case FILE_END:
		/*
		 * FILE_END: offset is typically negative (seeking backward
		 * from end). Add signed offset to file size.
		 */
		new_pos = fp->size + (int64_t)offset;
		break;

	default:
		pir->ir_error = ERROR_INVALID_FUNCTION;
		return pir->ir_error;
	}


	if (new_pos > fp->size) {
		new_pos = fp->size;
	}

	fp->pos = new_pos;
	pir->ir_pos = (ULONG) new_pos;

	return 0;
}


/*
 * apfs_close_file -- Free the apfs_file_t handle and reset position.
 */
int _cdecl apfs_close_file(pioreq pir) {
	apfs_file_t* fp;

	Debug_Printf("APFS: entering apfs_close_file\r\n");

	fp = (apfs_file_t*) pir->ir_fh;
	if (fp != NULL) {
		Debug_Printf("APFS: CloseFile: ino=0x%08lX%08lX\r\n",
		             (DWORD)(fp->ino >> 32),
		             (DWORD)(fp->ino & 0xFFFFFFFF));

		_HeapFree(fp, 0);
	}

	pir->ir_fh = 0;
	pir->ir_pos = 0;

	return 0;
}

/*
 * apfs_file_datetime -- Handle file timestamp get/set requests.
 * GET operations convert APFS timestamps via apfs_to_dos_time.  
 * SET operations are rejected (read-only volume).   
 *
 */
int _cdecl apfs_file_datetime(pioreq pir) {
	apfs_file_t* fp;
	int subfunc;

	Debug_Printf("APFS: entering apfs_file_datetime\r\n");

	fp = (apfs_file_t*) pir->ir_fh;
	if (fp == NULL) {
		pir->ir_error = ERROR_INVALID_HANDLE;
		return pir->ir_error;
	}

	subfunc = pir->ir_flags;

	switch (subfunc) {

	case GET_MODIFY_DATETIME:
		pir->ir_dostime = apfs_to_dos_time(fp->inode.mod_time);
		return 0;

	case SET_MODIFY_DATETIME:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case GET_CREATE_DATETIME:
		pir->ir_dostime = apfs_to_dos_time(fp->inode.create_time);
		return 0;

	case SET_CREATE_DATETIME:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	case GET_ACCESS_DATETIME:
		pir->ir_dostime = apfs_to_dos_time(fp->inode.access_time);
		return 0;

	case SET_ACCESS_DATETIME:
		pir->ir_error = ERROR_ACCESS_DENIED;
		return pir->ir_error;

	default:
		pir->ir_error = ERROR_INVALID_FUNCTION;
		return pir->ir_error;
	}
}

/*
 * apfs_find_next_file -- Return the next matching directory entry.
 *
 * Continues scanning from find->current_index through the cached
 * directory entries. Applies IFSMgr_MetaMatch pattern matching.
 * On match, fills the _WIN32_FIND_DATA and advances the index.
 * Returns ERROR_NO_MORE_FILES when no more entries match.
 */
int _cdecl apfs_find_next_file(pioreq pir) {
	apfs_find_t* find;
	_WIN32_FIND_DATA* fdp;
	uint32_t i;
	dir_entry_cache_t* e;
	int match_result;

	Debug_Printf("APFS: entering apfs_find_next_file\r\n");

	find = (apfs_find_t*) pir->ir_fh;
	if (find == NULL) {
		pir->ir_error = ERROR_FILE_NOT_FOUND;
		return pir->ir_error;
	}

	fdp = (_WIN32_FIND_DATA*) pir->ir_data;

	/* Scan from current_index for the next matching entry */
	for (i = find->current_index; i < find->entry_count; i++) {
		e = &find->entries[i];

		/* Apply pattern matching. IFSMgr_MetaMatch returns NON-ZERO on
		 * a match; UFLG_META makes '*'/'?' behave as wildcards. */
		match_result = IFSMgr_MetaMatch((string_t) find->pattern,
		                                (string_t) e->name,
		                                UFLG_NT_DOS | UFLG_META);
		if (match_result != 0) {
			fill_find_data(fdp, e);
			find->current_index = i + 1;
			return 0;
		}
	}

	/* No more matching entries */
	pir->ir_error = ERROR_NO_MORE_FILES;
	return pir->ir_error;
}

/*
 * apfs_find_close -- Free the find context and cached entries array.
 */
int _cdecl apfs_find_close(pioreq pir) {
	apfs_find_t* find;

	Debug_Printf("APFS: entering apfs_find_close\r\n");

	find = (apfs_find_t*) pir->ir_fh;
	if (find != NULL) {
		if (find->entries != NULL) {
			_HeapFree(find->entries, 0);
			find->entries = NULL;
		}

		_HeapFree(find, 0);
	}

	pir->ir_fh = 0;
	return 0;
}

/*
 * apfs_enumerate_handle -- Enumerate open handles on a volume.
 *
 * Not supported for APFS. Returns ERROR_INVALID_FUNCTION.
 */
int _cdecl apfs_enumerate_handle(pioreq pir) {
	Debug_Printf("APFS: entering apfs_enumerate_handle\r\n");
	pir->ir_error = ERROR_INVALID_FUNCTION;
	return pir->ir_error;
}
