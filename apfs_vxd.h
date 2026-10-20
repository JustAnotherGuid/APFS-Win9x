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
 * apfs_vxd.h -- Runtime structures and function prototypes
 *
 * Central header for the APFS VxD IFS driver. Includes DDK headers,
 * APFS type definitions, runtime data structures for mounted volumes,
 * open file handles, and directory search contexts, and declares the
 * public API for all driver modules.
 *
 * This file is included by every .c file in the driver. DDK headers
 * must come first (they define BYTE, USHORT, DWORD, ULONG, PVOID,
 * PVRP, dos_time, pioreq, volfunc, hndlmisc, hndlfunc, ParsedPath,
 * etc.). Our apfs_types.h provides the APFS on-disk structure
 * definitions with MSVC intrinsic integer typedefs.
 */

#ifndef APFS_VXD_H
#define APFS_VXD_H

/* ========================================================================== */
/* DDK headers                                                                */
/*                                                                            */
/* Order matters: WANTVXDWRAPS must be defined before basedef.h so that       */
/* vxdwraps.h provides C-callable wrappers for VMM services.                  */
/* ========================================================================== */

#ifndef WANTVXDWRAPS
#define WANTVXDWRAPS
#endif

#include <basedef.h>        /* BYTE, USHORT, DWORD, ULONG, PVOID, etc. */
#include <vmm.h>            /* VMM services */
#include <debug.h>          /* Debug_Printf */
#include <vxdwraps.h>       /* C wrappers: _HeapAllocate, _HeapFree, etc. */
#include <winerror.h>       /* Win32 error codes */
#include <ifs.h>            /* IFS types: pioreq, volfunc, hndlmisc, etc. */
#include <drp.h>            /* DRP structure */
#include <ilb.h>            /* ILB structure */
#include <ior.h>            /* IOR structure */
#include <isp.h>            /* ISP structures */
#include <aep.h>            /* AEP structures */
#include <vrp.h>            /* VRP structure */

/*
 * DCB forward declaration and PDCB typedef.
 *
 * We only use PDCB as an opaque pointer (passed to IOS_SendCommand
 * and extracted from VRP_device_handle). We never access DCB struct
 * members directly. A forward declaration avoids depending on dcb.h
 * which may not be available or may not typedef PDCB in all DDK
 * versions.
 */
struct _DCB;
typedef struct _DCB* PDCB;

/*
 * SGD (Scatter/Gather Descriptor).
 * Used in IOR allocation size calculations.
 */
typedef struct _SGD {
    ULONG SG_buff_ptr;
    ULONG SG_buff_size;
} SGD;

#include <string.h>         /* compiler intrinsic prototypes */
#pragma intrinsic(memcpy, memset, memcmp, strlen, strcmp)

/*
 * Maximum filename length in Unicode characters for stack buffers
 * used during path component matching.
 */
#define MAX_NAME_CHARS  256


/* ========================================================================== */
/* MBR Constants							                                  */
/* ========================================================================== */
#define MBR_SIGNATURE       0xAA55
#define MBR_SIG_OFFSET      510
#define MBR_PART_TYPE_GPT   0xEE
#define MBR_PART_TABLE_OFF  446
#define MBR_PART_ENTRY_SIZE 16
#define MBR_PART_TYPE_OFF   4
#define SECTOR_SIZE         512


/* ========================================================================== */
/* APFS and GPT on-disk type definitions                                      */
/* ========================================================================== */

#include "apfs_types.h"     /* APFS on-disk structures, uint8_t..uint64_t */
#include "gpt_types.h"      /* GPT header and partition entry structures */

/* ========================================================================== */
/* IOR_completion_flag compatibility                                          */
/*                                                                            */
/* The Win98 DDK IOR structure does not have an IOR_completion_flag field.    */
/* Map it to _ureq._IOR_requestor_usage[0], which							  */
/* is a ULONG in the requestor-usage area reserved for the FSD's use.         */
/* IOR_private_IOS is reserved by IOS itself -- do NOT use it.                */
/* ========================================================================== */

#ifndef IOR_completion_flag
#define IOR_completion_flag _ureq._IOR_requestor_usage[0]
#endif

#include "vxdwrap.h"        /* IOS/IFSMgr VxD service wrappers */

/* ========================================================================== */
/* Generic Constants                                                          */
/* ========================================================================== */
#ifndef NORMAL_FSD
#define NORMAL_FSD					0
#endif 

/* TODO: Validate with other 9x versions. */
#ifndef IFSMGRVERSION
#define IFSMGRVERSION				0x22
#endif 

/* ========================================================================== */
/* File Constants                                                             */
/* ========================================================================== */

#ifndef FILE_CURRENT
#define FILE_CURRENT				1
#endif

#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL		0x80
#endif


/* ========================================================================== */
/* DateTime Constants                                                         */
/* ========================================================================== */

#ifndef GET_MODIFY_DATETIME
#define GET_MODIFY_DATETIME			0
#endif

#ifndef SET_MODIFY_DATETIME
#define SET_MODIFY_DATETIME			1
#endif

#ifndef GET_CREATE_DATETIME
#define GET_CREATE_DATETIME			2
#endif

#ifndef SET_CREATE_DATETIME
#define SET_CREATE_DATETIME			3
#endif

#ifndef GET_ACCESS_DATETIME
#define GET_ACCESS_DATETIME			4
#endif

#ifndef SET_ACCESS_DATETIME
#define SET_ACCESS_DATETIME			5
#endif

#ifndef GET_ATTRIB_CREATE_DATETIME
#define GET_ATTRIB_CREATE_DATETIME  5
#endif

#ifndef SET_ATTRIB_CREATE_DATETIME
#define SET_ATTRIB_CREATE_DATETIME  6
#endif

#ifndef GET_ATTRIB_ACCESS_DATETIME
#define GET_ATTRIB_ACCESS_DATETIME  7
#endif

#ifndef SET_ATTRIB_ACCESS_DATETIME
#define SET_ATTRIB_ACCESS_DATETIME  8
#endif
 

/* ========================================================================== */
/* Forward declarations                                                       */
/* ========================================================================== */

struct apfs_volume;
struct apfs_file;
struct apfs_find;
struct dir_entry_cache;
struct block_cache;
struct block_cache_entry;

/* ========================================================================== */
/* Block cache (64-entry LRU)								                  */
/* ========================================================================== */

#define BLOCK_CACHE_SIZE    64      /* number of cached blocks */

typedef struct block_cache_entry {
    uint64_t    block_addr;         /* APFS block address (UINT64_MAX = empty) */
    uint32_t    age;                /* LRU counter value at last access */
    uint8_t*    data;               /* block_size bytes of block data */
} block_cache_entry_t;

typedef struct block_cache {
    uint32_t    num_entries;        /* number of entries in use */
    uint32_t    lru_counter;        /* monotonically increasing counter */
    uint32_t    block_size;         /* block size in bytes */
    block_cache_entry_t entries[BLOCK_CACHE_SIZE];
} block_cache_t;

/* ========================================================================== */
/* One instance per mounted APFS volume. Stored as the IFSMgr resource        */
/* handle (ir_rh) so every FSD callback can retrieve it.                      */
/* ========================================================================== */

typedef struct apfs_volume {
    struct apfs_volume* next;       /* linked list of mounted volumes */
    struct apfs_volume* prev;

    /* IOS interface (DDK types -- OS handles) */
    PVRP        vrp;                /* Volume Request Packet from IOS */
    BYTE        drive;              /* drive letter (0=A, 2=C, etc.) */
    BYTE        orig_drive;         /* original drive letter before remap */

    /* APFS container state (APFS types -- match on-disk widths) */
    uint32_t    block_size;         /* container block size (typically 4096) */
    uint64_t    partition_offset;   /* byte offset of APFS partition on disk */
    nx_superblock_t  container_sb;  /* latest valid container superblock */
    uint64_t    container_omap_root;/* physical block of container omap root */

    /* APFS volume state */
    apfs_superblock_t volume_sb;    /* volume superblock */
    uint64_t    volume_omap_root;   /* physical block of volume omap root */
    uint64_t    fs_tree_root;       /* physical block of FS B-tree root */
    uint64_t    max_xid;            /* highest valid transaction ID */

    /* Volume metadata (cached for GetDiskInfo / QueryResourceInfo) */
    char        vol_label[256];     /* UTF-8 volume name (from APFS) */
    USHORT      vol_label_uni[256]; /* Unicode volume name (for IFSMgr) */
    uint32_t    vol_serial;         /* synthetic serial number */

    /* Block cache */
    struct block_cache* cache;      /* simple LRU block cache */
} apfs_volume_t;


/* ========================================================================== */
/* Per-open-file handle context						                          */
/*                                                                            */
/* One instance per open file handle. Stored as the IFSMgr file handle        */
/* (ir_fh) so handle-level callbacks can retrieve it.                         */
/* ========================================================================== */

typedef struct apfs_file {
    apfs_volume_t*    vol;          /* owning volume */

    uint64_t    ino;                /* APFS inode number */
    uint64_t    private_id;         /* private_id for extent lookups */
    uint64_t    size;               /* total file size from dstream */
    uint64_t    pos;                /* current seek position */

    /* Cached inode metadata */
    j_inode_val_t inode;            /* inode record (fixed portion) */
    uint8_t     d_type;             /* DT_REG, DT_DIR, etc. */
    uint16_t    mode;               /* Unix mode bits */
} apfs_file_t;

/* ========================================================================== */
/* Per-FindFirst/FindNext search context					                  */
/*                                                                            */
/* Tracks directory enumeration state across FindFirst -> FindNext ->         */
/* FindClose. We snapshot the directory into a temporary list at              */
/* FindFirst time and iterate it on subsequent FindNext calls.                */
/* ========================================================================== */

typedef struct apfs_find {
    apfs_volume_t* vol;             /* owning volume */
    uint64_t    dir_ino;            /* directory inode being searched */
    DWORD       attr_mask;          /* attribute filter from ir_attr */
    BYTE        exclude_mask;       /* attributes to exclude */
    int         match_semantics;    /* UFLG_DOS | UFLG_META | UFLG_NT */
    USHORT      pattern[256];       /* Unicode search pattern */

    /* Directory snapshot state */
    uint32_t    entry_count;        /* total entries found */
    uint32_t    current_index;      /* next entry to return */
    struct dir_entry_cache* entries; /* cached directory entries array */
} apfs_find_t;

/* ========================================================================== */
/* Cached directory entry for FindFirst/FindNext					          */
/* ========================================================================== */

typedef struct dir_entry_cache {
    uint64_t    ino;                /* target inode number */
    uint64_t    size;               /* file size (0 for directories) */
    uint16_t    mode;               /* Unix mode */
    uint8_t     d_type;             /* DT_REG, DT_DIR, etc. */
    BYTE        attr;               /* DOS attribute byte (synthesized) */
    dos_time    created;            /* DOS-format created timestamp */
    dos_time    modified;           /* DOS-format modified timestamp */
    dos_time    accessed;           /* DOS-format accessed timestamp */
    uint16_t    name_len;           /* name length in Unicode chars */
    USHORT      name[256];          /* Unicode/UCS-2 filename */
    USHORT      short_name[13];     /* 8.3 short name (generated) */
} dir_entry_cache_t;


/* ========================================================================== */
/* B-tree visitor callback typedef                                            */
/*                                                                            */
/* Called for each matching leaf record during B-tree lookup or               */
/* enumeration. Return 0 to continue, non-zero to stop.                      */
/* ========================================================================== */

typedef int (*btree_visitor_fn)(const void* key, uint16_t key_len,
                                const void* val, uint16_t val_len,
                                void* user_data);

/* ========================================================================== */
/* Function prototypes -- apfs_checksum.c                                     */
/* ========================================================================== */

/*
 * apfs_fletcher64 -- Compute Fletcher-64 checksum over a buffer.
 * buf:        Pointer to data (treated as array of uint32_t)
 * len:        Length in bytes (must be multiple of 4)
 * Returns:    64-bit Fletcher-64 checksum value
 */
uint64_t apfs_fletcher64(const void* buf, uint32_t len);

/*
 * apfs_verify_block -- Verify Fletcher-64 checksum of an APFS object block.
 * block_buf:  Pointer to the block data (checksum in first 8 bytes)
 * block_size: Size of the block in bytes
 * Returns:    0 on success, -1 on checksum mismatch
 */
int apfs_verify_block(const void* block_buf, uint32_t block_size);

/* ========================================================================== */
/* Function prototypes -- apfs_blockio.c                                      */
/* ========================================================================== */

/*
 * cache_init -- Allocate and initialize a block cache for a volume.
 * block_size: Size of each cached block in bytes
 * Returns:    Pointer to new block_cache_t, or NULL on failure
 */
block_cache_t* cache_init(uint32_t block_size);

/*
 * cache_destroy -- Free all cache buffers and the cache structure.
 * cache:      Pointer to block_cache_t (may be NULL)
 */
void cache_destroy(block_cache_t* cache);

/*
 * cache_find -- Look up a block in the cache.
 * cache:      pointer to block_cache_t
 * block_addr: APFS block address to find
 * Returns:    Pointer to cached block data, or NULL if not cached
 */
uint8_t* cache_find(block_cache_t* cache, uint64_t block_addr);

/*
 * cache_invalidate_all -- Mark all cache entries as empty.
 * cache:      pointer to block_cache_t (may be NULL)
 */
void cache_invalidate_all(block_cache_t* cache);

/*
 * apfs_read_block -- Read a single APFS block via IOS, with caching.
 * vol:        Pointer to mounted volume context
 * block_addr: APFS block address to read
 * buf:        Output buffer (must be at least vol->block_size bytes)
 * Returns:    0 on success, Win32 error code on failure
 */
int apfs_read_block(apfs_volume_t* vol, uint64_t block_addr, void* buf);

/*
 * apfs_read_blocks -- Read multiple contiguous APFS blocks via IOS.
 * vol:        Pointer to mounted volume context
 * block_addr: Starting APFS block address
 * count:      Number of blocks to read
 * buf:        Output buffer (must be at least count * vol->block_size bytes)
 * Returns:    0 on success, Win32 error code on failure
 */
int apfs_read_blocks(apfs_volume_t* vol, uint64_t block_addr,
                     uint32_t count, void* buf);


/* ========================================================================== */
/* Function prototypes -- apfs_gpt.c                                          */
/* ========================================================================== */

/*
 * apfs_gpt_find_partition -- Scan GPT for an APFS partition.
 * vol:        Pointer to volume context (uses vol->vrp for sector reads)
 * offset_out: Receives the byte offset of the APFS partition on disk
 * Returns:    0 if APFS partition found, -1 on error or not found
 */
int apfs_gpt_find_partition(apfs_volume_t* vol, uint64_t* offset_out);

/* ========================================================================== */
/* Function prototypes -- apfs_container.c                                    */
/* ========================================================================== */

/*
 * apfs_container_open -- Read and validate the APFS container superblock.
 * vol:        Pointer to volume context (block_size, partition_offset,
 *             container_sb, container_omap_root are filled in)
 * Returns:    0 on success, -1 on failure
 */
int apfs_container_open(apfs_volume_t* vol);

/* ========================================================================== */
/* Function prototypes -- apfs_omap.c                                         */
/* ========================================================================== */

/*
 * apfs_omap_lookup -- Resolve a virtual OID to a physical block address.
 * vol:        Pointer to volume context (for block reads)
 * omap_root:  Physical block address of the omap B-tree root
 * oid:        Virtual object identifier to resolve
 * max_xid:    Maximum transaction ID to consider
 * out_paddr:  Receives the resolved physical block address
 * Returns:    0 on success, -1 if not found or error
 */
int apfs_omap_lookup(apfs_volume_t* vol, uint64_t omap_root,
                     uint64_t oid, uint64_t max_xid,
                     uint64_t* out_paddr);

/* ========================================================================== */
/* Function prototypes -- apfs_btree.c                                        */
/* ========================================================================== */

/*
 * apfs_btree_lookup -- Search a B-tree for records matching a key.
 * vol:        Pointer to volume context
 * root_block: Physical block address of the B-tree root node
 * search_key: Key to search for (prefix match)
 * key_len:    Length of the search key in bytes
 * visitor:    Callback invoked for each matching leaf record
 * user_data:  Opaque context passed to the visitor
 * Returns:    0 on success, -1 on error
 *
 * Uses vol->volume_omap_root and vol->max_xid for virtual child
 * resolution. For physical tree lookups (e.g., omap), use
 * apfs_btree_lookup_physical() instead.
 */
int apfs_btree_lookup(apfs_volume_t* vol, uint64_t root_block,
                      const void* search_key, uint16_t key_len,
                      btree_visitor_fn visitor, void* user_data);

/*
 * apfs_btree_lookup_physical -- Search a physical B-tree (no omap
 *                              resolution for child pointers).
 * vol:        Pointer to volume context (for block reads only)
 * root_block: Physical block address of the B-tree root node
 * search_key: Key to search for (prefix match)
 * key_len:    Length of the search key in bytes
 * visitor:    Callback invoked for each matching leaf record
 * user_data:  Opaque context passed to the visitor
 * Returns:    0 on success, -1 on error
 *
 * Used by apfs_omap_lookup() to search the omap B-tree itself.
 * Child OIDs are treated as direct physical block addresses.
 */
int apfs_btree_lookup_physical(apfs_volume_t* vol, uint64_t root_block,
                               const void* search_key, uint16_t key_len,
                               btree_visitor_fn visitor, void* user_data);

/*
 * apfs_btree_enumerate -- Enumerate all B-tree records matching an OID
 *                        and record type.
 * vol:         Pointer to volume context
 * root_block:  Physical block address of the B-tree root node
 * oid:         Object identifier to match
 * record_type: APFS_TYPE_* record type to match
 * visitor:     Callback invoked for each matching leaf record
 * user_data:   Opaque context passed to the visitor
 * Returns:     0 on success, -1 on error
 */
int apfs_btree_enumerate(apfs_volume_t* vol, uint64_t root_block,
                         uint64_t oid, uint8_t record_type,
                         btree_visitor_fn visitor, void* user_data);

/* ========================================================================== */
/* Function prototypes -- apfs_volume.c                                       */
/* ========================================================================== */

/*
 * apfs_volume_open -- Open an APFS volume within a container.
 * vol:        Pointer to volume context (container must be open)
 * vol_index:  Index of the volume to open (0 = first volume)
 * Returns:    0 on success, -1 on failure
 */
int apfs_volume_open(apfs_volume_t* vol, uint32_t vol_index);


/* ========================================================================== */
/* Function prototypes -- apfs_fsrec.c                                        */
/* ========================================================================== */

/*
 * apfs_lookup_inode -- Look up an inode record by inode number.
 * vol:        Pointer to volume context
 * ino:        APFS inode number
 * out:        Receives the inode value record
 * Returns:    0 on success, -1 if not found or error
 */
int apfs_lookup_inode(apfs_volume_t* vol, uint64_t ino,
                      j_inode_val_t* out);

/*
 * apfs_list_dir -- Enumerate directory entries for a directory inode.
 * vol:        Pointer to volume context
 * dir_ino:    Inode number of the directory
 * visitor:    Callback invoked for each DIR_REC record
 * user_data:  Opaque context passed to the visitor
 * Returns:    0 on success, -1 on error
 */
int apfs_list_dir(apfs_volume_t* vol, uint64_t dir_ino,
                  btree_visitor_fn visitor, void* user_data);

/*
 * apfs_list_extents -- Enumerate file extent records for a file.
 * vol:        Pointer to volume context
 * private_id: Private_id of the file (from inode record)
 * visitor:    Callback invoked for each FILE_EXTENT record
 * user_data:  Opaque context passed to the visitor
 * Returns:    0 on success, -1 on error
 */
int apfs_list_extents(apfs_volume_t* vol, uint64_t private_id,
                      btree_visitor_fn visitor, void* user_data);

/*
 * apfs_extract_dstream -- Extract data stream info from inode xfields.
 * val_buf:    Pointer to the full inode value record (including xfields)
 * val_len:    Total length of the value record in bytes
 * out:        Receives the j_dstream_t data
 * Returns:    0 on success, -1 if no dstream field found
 */
int apfs_extract_dstream(const void* val_buf, uint16_t val_len,
                         j_dstream_t* out);

/* ========================================================================== */
/* Function prototypes -- apfs_time.c                                         */
/* ========================================================================== */

/*
 * apfs_to_dos_time -- Convert APFS nanosecond timestamp to DOS time format.
 * apfs_ns:    APFS timestamp (nanoseconds since 2000-01-01 00:00:00 UTC)
 * Returns:    dos_time structure with date and time fields
 */
dos_time apfs_to_dos_time(uint64_t apfs_ns);

/* ========================================================================== */
/* Function prototypes -- apfs_names.c                                        */
/* ========================================================================== */

/*
 * apfs_name_to_unicode -- Convert an APFS UTF-8 filename to UCS-2 Unicode.
 * utf8_name:  Pointer to the UTF-8 name bytes
 * utf8_len:   Length of the UTF-8 name in bytes (excluding null terminator)
 * uni_out:    Output buffer for UCS-2 characters
 * uni_max:    Maximum number of USHORT characters in uni_out
 * Returns:    Number of Unicode characters written, or -1 on error
 */
int apfs_name_to_unicode(const uint8_t* utf8_name, uint16_t utf8_len,
                         USHORT* uni_out, uint16_t uni_max);

/*
 * apfs_names_match_ci -- Case-insensitive Unicode name comparison.
 * name1:      First Unicode string
 * len1:       Length of name1 in characters
 * name2:      Second Unicode string
 * len2:       Length of name2 in characters
 * Returns:    0 if names match (case-insensitive), non-zero otherwise
 */
int apfs_names_match_ci(const USHORT* name1, uint16_t len1,
                        const USHORT* name2, uint16_t len2);

/*
 * apfs_generate_short_name -- Generate an 8.3 DOS short name.
 * long_name:  Unicode long filename
 * name_len:   Length of long_name in characters
 * short_out:  Output buffer for 8.3 name (at least 13 USHORTs)
 */
void apfs_generate_short_name(const USHORT* long_name, uint16_t name_len,
                              USHORT* short_out);

/*
 * apfs_synthesize_dos_attr -- Synthesize DOS attributes from APFS metadata.
 * d_type:     Directory entry type (DT_REG, DT_DIR, etc.)
 * mode:       Unix mode bits
 * name:       Unicode filename (for dot-prefix detection)
 * name_len:   Length of name in characters
 * Returns:    DOS attribute byte (FILE_ATTRIBUTE_*)
 */
BYTE apfs_synthesize_dos_attr(uint8_t d_type, uint16_t mode,
                              const USHORT* name, uint16_t name_len);

/* ========================================================================== */
/* Function prototypes -- apfs_resolve.c                                      */
/* ========================================================================== */


/*
 * apfs_resolve_path -- Walk a ParsedPath to resolve a file or directory.
 * vol:        Pointer to volume context
 * path:       IFSMgr ParsedPath structure
 * out_ino:    Receives the resolved inode number
 * out_dtype:  Receives the directory entry type (DT_REG, DT_DIR, etc.)
 * Returns:    0 on success, Win32 error code on failure
 */
int apfs_resolve_path(apfs_volume_t* vol, ParsedPath* path,
                      uint64_t* out_ino, uint8_t* out_dtype);



/* ========================================================================== */
/* Function prototypes -- apfs_utils.c                                        */
/* ========================================================================== */

/*
 * fill_find_data -- Fill a _WIN32_FIND_DATA structure from a cached
 * directory entry.
 * fdp:			Pointer Win32 find data structure.
 * entry:		Pointer to directory entry cache
 */
void fill_find_data(_WIN32_FIND_DATA* fdp, dir_entry_cache_t* entry);

/*
 * apfs_inode_info -- Look up an inode and return its logical size.
 *
 * vol:			Pointer to volume context
 * ino:			Inode number to get info for
 * out_inode	Output inode value
 * out_size		Output size of inode value
 */
int apfs_inode_info(apfs_volume_t* vol, uint64_t ino, j_inode_val_t* out_inode, uint64_t* out_size);


/* ========================================================================== */
/* Function prototypes -- apfs_dispatch.c                                     */
/* ========================================================================== */
/* Note these functions are cdecls because IFS will call them directly
 * the functions in this file and in apfs_handle.c are basically just
 * the functions IFS in Win9x expects. 
 */
int _cdecl apfs_delete_file(pioreq pir);
int _cdecl apfs_dir(pioreq pir);
int _cdecl apfs_file_attributes(pioreq pir);
int _cdecl apfs_flush_volume(pioreq pir);
int _cdecl apfs_get_disk_info(pioreq pir);
int _cdecl apfs_open_file(pioreq pir);
int _cdecl apfs_rename_file(pioreq pir);
int _cdecl apfs_search_file(pioreq pir);
int _cdecl apfs_query_resource_info(pioreq pir);
int _cdecl apfs_disconnect(pioreq pir);
int _cdecl apfs_empty_func(pioreq pir);
int _cdecl apfs_ioctl16_drive(pioreq pir);
int _cdecl apfs_get_disk_parms(pioreq pir);
int _cdecl apfs_find_first_file(pioreq pir);
int _cdecl apfs_direct_disk_io(pioreq pir);

/* ========================================================================== */
/* Function prototypes -- apfs_handle.c                                       */
/* ========================================================================== */
int _cdecl apfs_read_file(pioreq pir);
int _cdecl apfs_write_file(pioreq pir);
int _cdecl apfs_file_seek(pioreq pir);
int _cdecl apfs_close_file(pioreq pir);
int _cdecl apfs_file_datetime(pioreq pir);
int _cdecl apfs_find_next_file(pioreq pir);
int _cdecl apfs_find_close(pioreq pir);
int _cdecl apfs_enumerate_handle(pioreq pir);

#endif /* APFS_VXD_H */