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
 * gpt_types.h -- GPT on-disk structure definitions
 *
 * GUID Partition Table (GPT) header and partition entry structures
 * for locating APFS partitions on USB mass storage devices.
 *
 * All multi-byte integer fields are stored in little-endian byte order
 * as specified by the UEFI specification.
 */

#ifndef GPT_TYPES_H
#define GPT_TYPES_H

#include "apfs_types.h"     /* uint8_t .. uint64_t typedefs */

/* ========================================================================== */
/* GPT constants                                                              */
/* ========================================================================== */

/* "EFI PART" signature stored as a little-endian uint64_t */
#define GPT_SIGNATURE   ((uint64_t)0x54524150 << 32 | (uint64_t)0x20494645)

/*
 * APFS partition type GUID in mixed-endian GPT byte order.
 * GUID: 7C3457EF-0000-11AA-AA11-00306543ECAC
 *
 * GPT stores GUIDs in mixed-endian format:
 *   - First 3 components (Data1, Data2, Data3) are little-endian
 *   - Last 2 components (Data4) are big-endian (byte order preserved)
 */
static const uint8_t APFS_GPT_GUID[16] = {
    0xEF, 0x57, 0x34, 0x7C,                   
    0x00, 0x00,                                 
    0xAA, 0x11,                                 
    0xAA, 0x11,                                 
    0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC         
};

/* ========================================================================== */
/* On-disk structures (all packed, little-endian)                             */
/* ========================================================================== */

#pragma pack(push, 1)

/* GPT Header -- located at LBA 1 (byte offset 512) */
typedef struct {
    uint64_t signature;                 /* "EFI PART" = 0x5452415020494645 */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

/* GPT Partition Entry -- 128 bytes each in the partition entry array */
typedef struct {
    uint8_t  partition_type_guid[16];
    uint8_t  unique_partition_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name[36];                  /* UTF-16LE partition name */
} gpt_partition_entry_t;

#pragma pack(pop)

#endif /* GPT_TYPES_H */
