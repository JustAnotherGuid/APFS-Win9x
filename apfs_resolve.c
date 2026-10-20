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
 * apfs_resolve.c -- Resolves ParsedPaths to inodes
 *
 * Uses ParsedPath elements to find corresponding Inodes
 * within the file system storage medium. 
 */

#include "apfs_vxd.h"

/*
 * path_resolve_ctx_t -- Visitor context for directory entry matching
 *                      during path resolution.
 *
 * The visitor converts each DIR_REC name from UTF-8 to Unicode/UCS-2,
 * then compares case-insensitively against the target component.
 * On match, it records the target inode number and d_type.
 */
typedef struct {
	const USHORT*   target_name;    /* Unicode name to match */
	uint16_t        target_len;     /* length in characters */
	uint64_t        found_ino;      /* matched inode number */
	uint8_t         found_dtype;    /* matched d_type */
	int             found;          /* non-zero if match found */
} path_resolve_ctx_t;

/*
 * path_resolve_visitor -- B-tree visitor callback for DIR_REC matching.
 *
 * Called for each directory entry in the current directory. Extracts
 * the UTF-8 name from the j_drec_key_t, converts to Unicode, and
 * compares case-insensitively against the target path component.
 *
 * Returns 1 to stop enumeration on match, 0 to continue.
 */
static int path_resolve_visitor(const void* key, uint16_t key_len,
                                const void* val, uint16_t val_len,
                                void* user_data) {
	path_resolve_ctx_t* ctx;
	const j_drec_key_t* drec_key;
	const j_drec_val_t* drec_val;
	const uint8_t* name_bytes;
	uint16_t name_len;
	USHORT uni_buf[MAX_NAME_CHARS];
	int uni_len;

	ctx = (path_resolve_ctx_t*)user_data;

	if (key_len < sizeof(j_drec_key_t)) return 0;
	if (val_len < sizeof(j_drec_val_t)) return 0;

	drec_key = (const j_drec_key_t*)key;
	drec_val = (const j_drec_val_t*)val;

	/*
	 * Extract the name from the directory record key.
	 * The name bytes follow immediately after the j_drec_key_t header.
	 * name_len includes the null terminator, so actual string length
	 * is name_len - 1.
	 */
	name_len = (uint16_t)(drec_key->name_len_and_hash & J_DREC_LEN_MASK);
	if (name_len == 0) return 0;  /* skip empty name */
	name_bytes = (const uint8_t*)key + sizeof(j_drec_key_t);

	/* Subtract null terminator from length for conversion */
	if (name_len > 1) {
		name_len = name_len - 1;
	} else {
		return 0;  /* skip -- only null terminator */
	}

	/* Verify the name bytes are within the key buffer */
	if ((uint16_t)(sizeof(j_drec_key_t) + name_len) > key_len) return 0;

	Debug_Printf("APFS: path_resolve_visitor: Visited: %s\n", name_bytes);
	uni_len = apfs_name_to_unicode(name_bytes, name_len, uni_buf, MAX_NAME_CHARS);
	if (uni_len <= 0) return 0;
	
	Debug_Printf("APFS: path_resolve_visitor: Target length: %d, Converted Length: %d\n", ctx->target_len, uni_len);
	uni_buf[uni_len] = 0;
	/* Case-insensitive comparison */
	if (apfs_names_match_ci(uni_buf, (uint16_t)uni_len, ctx->target_name, ctx->target_len) == 0) {
		ctx->found_ino = drec_val->file_id;
		ctx->found_dtype = (uint8_t)(drec_val->flags & DREC_TYPE_MASK);
		ctx->found = 1;
		return 1;  /* stop enumeration */
	}

	return 0;  /* continue */
}



/*
 * apfs_resolve_path -- Walk a ParsedPath to resolve a file or directory.
 *
 * Iterates through each component of the IFSMgr ParsedPath structure,
 * starting at the root directory inode (APFS_ROOT_DIR_INO = 2). For
 * each component, enumerates DIR_REC entries in the current directory,
 * converts names from UTF-8 to Unicode, and compares case-insensitively.
 *
 * Special cases:
 *   - Root path (IFSIsRoot): returns inode 2 with DT_DIR immediately
 *   - Intermediate component not found: ERROR_PATH_NOT_FOUND
 *   - Intermediate component not a directory: ERROR_PATH_NOT_FOUND
 *   - Final component not found: ERROR_FILE_NOT_FOUND
 *
 * vol:        pointer to mounted volume context
 * path:       IFSMgr ParsedPath structure
 * out_ino:    receives the resolved inode number
 * out_dtype:  receives the directory entry type (DT_REG, DT_DIR, etc.)
 * Returns:    0 on success, Win32 error code on failure
 */
int apfs_resolve_path(apfs_volume_t* vol, ParsedPath* path,
                      uint64_t* out_ino, uint8_t* out_dtype) {
	uint64_t current_ino;
	PathElement* pe;
	PathElement* end;
	PathElement* next_pe;
	uint16_t name_len;
	path_resolve_ctx_t ctx;
	int rc;
	int is_last;		

	if (vol == NULL || path == NULL || out_ino == NULL || out_dtype == NULL) return ERROR_PATH_NOT_FOUND;

	/*
	 * Root path special case.
	 * IFSIsRoot() checks pp_totalLength == 4, meaning the path
	 * contains only the 2-byte totalLength and 2-byte prefixLength
	 * fields with no path elements.
	 */
	if (IFSIsRoot(path)) {
		*out_ino = APFS_ROOT_DIR_INO;
		*out_dtype = DT_DIR;
		return 0;
	}

	current_ino = APFS_ROOT_DIR_INO;
	ctx.found_dtype = DT_DIR;
	ctx.found = 0;

	/*
	 * Walk path elements starting at the first real path component.
	 * pp_elements[0] is the FIRST element; subsequent elements are
	 * reached via IFSNextElement(). (IFSLastElement() returns the LAST
	 * element. Elements end when we reach pp_totalLength bytes
	 * from the start of the ParsedPath + sizeof(pp_totalLength):
	 */
	pe = &path->pp_elements[0];
	end = (PathElement*)((char*)path + path->pp_totalLength + sizeof(path->pp_totalLength));

	while ((char*)pe < (char*)end) {
		/*
		 * Compute the name length in Unicode (a.k.a UCS-2) characters.
		 * pe_length includes the 2-byte length field itself,
		 * so the name data is (pe_length - 2) bytes, and each
		 * Unicode character is 2 bytes (sizeof(USHORT)).
		 */
		if (pe->pe_length <= sizeof(pe->pe_length)) break;
		name_len = (uint16_t)((pe->pe_length - sizeof(pe->pe_length)) / sizeof(USHORT));
		if (name_len == 0) break;
		

		next_pe = IFSNextElement(pe);
		/* Note the last token in this is actually the just
		 * the zero-length terminal token (see figure 16-3
		 * in Windows 95 Systems Programming, page 590).
		 */
		is_last = (((char*)next_pe >= (char*)end) || next_pe->pe_length <= sizeof(next_pe->pe_length)) ? 1 : 0;

		ctx.target_name = pe->pe_unichars;
		ctx.target_len = name_len;
		ctx.found_ino = 0;
		ctx.found_dtype = DT_UNKNOWN;
		ctx.found = 0;

		/* Enumerate DIR_REC entries for the current directory */
		rc = apfs_list_dir(vol, current_ino, path_resolve_visitor, &ctx);
		if (rc != 0) {
			Debug_Printf("APFS: resolve_path: list_dir failed "
			             "for ino 0x%08lX%08lX\r\n",
			             (DWORD)(current_ino >> 32),
			             (DWORD)(current_ino & 0xFFFFFFFF));
			return is_last ? ERROR_FILE_NOT_FOUND
			               : ERROR_PATH_NOT_FOUND;
		}

		if (!ctx.found) {
			/*
			 * Component not found.
			 * Intermediate -> ERROR_PATH_NOT_FOUND
			 * Final        -> ERROR_FILE_NOT_FOUND
			 */
			return is_last ? ERROR_FILE_NOT_FOUND
			               : ERROR_PATH_NOT_FOUND;
		}

		/*
		 * If this is not the last component, the matched entry
		 * must be a directory so we can descend into it.
		 */
		if (!is_last && ctx.found_dtype != DT_DIR) {
			return ERROR_PATH_NOT_FOUND;
		}

		current_ino = ctx.found_ino;
		pe = next_pe;
	}

	*out_ino = current_ino;
	*out_dtype = ctx.found_dtype;
	return 0;
}