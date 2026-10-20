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
 * apfs_names.c -- Name conversion and matching
 *
 * Converts APFS UTF-8 filenames to UCS-2 Unicode, performs
 * case-insensitive comparison, generates 8.3 DOS short names,
 * and synthesizes DOS attributes from APFS metadata.
 *
 * IFSMgr services used:
 *   BCSToUni()        -- byte character set to Unicode conversion
 *   UniToUpper()      -- Unicode uppercase conversion
 *   CreateBasis()     -- 8.3 basis name generation from long name
 *   AppendBasisTail() -- append ~N numeric tail to basis name
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

/*
 * BCS_WANSI -- ANSI character set identifier for BCSToUni().
 * Handles single-byte Latin characters. 
 */
#define BCS_WANSI   0

/*
 * 8.3 short name buffer size: 8 + 1 (dot) + 3 + 1 (null) = 13 USHORTs.
 */
#define SHORT_NAME_BUF  13

/*
 * Unicode dot character for hidden-file detection.
 */
#define UNICODE_DOT     0x002E

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_name_to_unicode -- Convert an APFS UTF-8 filename to UCS-2 Unicode.
 *
 * APFS stores filenames as UTF-8. IFSMgr works with UCS-2 Unicode
 * internally. We use BCSToUni() with BCS_WANSI to perform the
 * conversion. BCSToUni returns the number of bytes written to the
 * output buffer (each Unicode character is 2 bytes).
 *
 * utf8_name:  pointer to the UTF-8 name bytes
 * utf8_len:   length of the UTF-8 name in bytes (excluding null terminator)
 * uni_out:    output buffer for UCS-2 characters
 * uni_max:    maximum number of USHORT characters in uni_out
 * Returns:    number of Unicode characters written, or -1 on error
 */
int apfs_name_to_unicode(const uint8_t* utf8_name, uint16_t utf8_len,
                         USHORT* uni_out, uint16_t uni_max) {
	UINT bytes_written;

	if (!utf8_name || !uni_out || uni_max == 0) return -1;
	if (utf8_len == 0) return 0;


	if (utf8_len > uni_max) utf8_len = uni_max;

	bytes_written = BCSToUni((string_t)uni_out, (PBYTE)utf8_name, (UINT)utf8_len, BCS_WANSI);
	if (bytes_written == 0 && utf8_len > 0) {
		return -1;
	}

	/* BCSToUni returns bytes written; divide by 2 (a.k.a. sizeof(USHORT) for character count */
	return (int)(bytes_written / 2);
}

/*
 * apfs_names_match_ci -- Case-insensitive Unicode name comparison.
 *
 * Converts both names to uppercase using UniToUpper(), then compares
 * the uppercased versions byte-by-byte. This matches the default
 * APFS case-insensitive behavior without using the on-disk hash.
 *
 * name1:  first Unicode string
 * len1:   length of name1 in characters
 * name2:  second Unicode string
 * len2:   length of name2 in characters
 * Returns: 0 if names match (case-insensitive), non-zero otherwise
 */
int apfs_names_match_ci(const USHORT* name1, uint16_t len1,
                        const USHORT* name2, uint16_t len2) {
	USHORT upper1[MAX_NAME_CHARS];
	USHORT upper2[MAX_NAME_CHARS];
	uint16_t cmp_len1;
	uint16_t cmp_len2;
	uint16_t i;

	if (!name1 || !name2) return -1;
	if (len1 != len2) return 1;
	if (len1 == 0) return 0;

	cmp_len1 = (len1 > MAX_NAME_CHARS) ? MAX_NAME_CHARS : len1;
	cmp_len2 = (len2 > MAX_NAME_CHARS) ? MAX_NAME_CHARS : len2;

	memset(upper1, 0, sizeof(upper1));
	memset(upper2, 0, sizeof(upper2));
	UniToUpper((string_t)upper1, (string_t)name1, (UINT)(cmp_len1 * sizeof(USHORT)));
	UniToUpper((string_t)upper2, (string_t)name2, (UINT)(cmp_len2 * sizeof(USHORT)));

	/* Compare the uppercased versions character by character */
	for (i = 0; i < cmp_len1; i++) {
		if (upper1[i] != upper2[i]) {
			return 1;
		}
	}

	return 0;
}

/*
 * apfs_generate_short_name -- Generate an 8.3 DOS short name.
 *
 * Uses CreateBasis() to produce the 8.3 basis from the long Unicode
 * name, then AppendBasisTail() to add a ~1 suffix. This ensures
 * the short name is unique-ish and follows DOS naming conventions.
 * Although as things currently stand this function is broken in that
 * it does not add the correct numerical suffix.
 *
 * long_name:  Unicode long filename
 * name_len:   length of long_name in characters
 * short_out:  output buffer for 8.3 name (at least 13 USHORTs)
 */
void apfs_generate_short_name(const USHORT* long_name, uint16_t name_len,
                              USHORT* short_out) {
	USHORT basis[SHORT_NAME_BUF];
	int i;

	if (!long_name || !short_out || name_len == 0) {
		if (short_out) {
			short_out[0] = 0;
		}
		return;
	}

	/* Zero out the basis buffer */
	for (i = 0; i < SHORT_NAME_BUF; i++) {
		basis[i] = 0;
	}


	CreateBasis((string_t)basis, (string_t)long_name, (UINT)(name_len * sizeof(USHORT)));

	/*
	 * TODO: This needs to be done properly i.e. adding the right suffix.
	 * Although it isn't clear what the best way is without looping
	 * over the entries or substantially changing the visitor to
	 * keep track of common prefixes to be able to increment whenever
	 * a new entry shows up. Potentially hashing the prefix and incrementing
	 * on matches. 
	 */
	AppendBasisTail((string_t)basis, 1);
	

	/* Copy the result to the output buffer */
	for (i = 0; i < SHORT_NAME_BUF; i++) {
		short_out[i] = basis[i];
	}
}

/*
 * apfs_synthesize_dos_attr -- Synthesize DOS attributes from APFS metadata.
 *
 * APFS uses Unix-style permissions and directory entry types. We map
 * these to DOS file attributes:
 *
 *   DT_DIR                    -> FILE_ATTRIBUTE_DIRECTORY
 *   No write permission       -> FILE_ATTRIBUTE_READONLY
 *   Name starts with '.'      -> FILE_ATTRIBUTE_HIDDEN
 *   Default (no special attr) -> FILE_ATTRIBUTE_NORMAL
 *
 * FILE_ATTRIBUTE_NORMAL is only set when no other attributes apply.
 *
 * d_type:    directory entry type (DT_REG, DT_DIR, etc.)
 * mode:      Unix mode bits from the inode
 * name:      Unicode filename (for dot-prefix detection)
 * name_len:  length of name in characters
 * Returns:   DOS attribute byte
 */
BYTE apfs_synthesize_dos_attr(uint8_t d_type, uint16_t mode,
                              const USHORT* name, uint16_t name_len) {
	BYTE attr;

	attr = 0;

	/* Directories get the directory attribute */
	if (d_type == DT_DIR) {
		attr |= FILE_ATTRIBUTE_DIRECTORY;
	}

	/* No write permission -> read-only */
	if ((mode & 0222) == 0) {
		attr |= FILE_ATTRIBUTE_READONLY;
	}

	/* Unix hidden file convention: name starts with '.' */
	if (name && name_len > 0 && name[0] == UNICODE_DOT) {
		attr |= FILE_ATTRIBUTE_HIDDEN;
	}

	/*
	 * FILE_ATTRIBUTE_NORMAL is only valid when used alone.
	 * If no other attributes were set, use NORMAL.
	 */
	if (attr == 0) {
		attr = FILE_ATTRIBUTE_NORMAL;
	}

	return attr;
}
