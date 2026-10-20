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
 * apfs_types.h -- APFS on-disk structure definitions
 *
 * All structures defined from the Apple File System Reference.
 * Uses MSVC intrinsic integer typedefs and #pragma pack for the
 * ring-0 VxD build environment.
 *
 * All multi-byte integer fields are stored in little-endian byte order
 * as specified by the Apple File System Reference.
 */

#ifndef APFS_TYPES_H
#define APFS_TYPES_H

/* ========================================================================== */
/* MSVC intrinsic integer typedefs                                            */
/*                                                                            */
/* __int8 .. __int64 are compiler built-ins available in all versions of the  */
/* MSVC ring-0 compiler without any header dependency.  We typedef them to    */
/* the C99-style names used throughout the Apple File System Reference and    */
/* our reader codebase, so every on-disk structure definition can be kept     */
/* verbatim.                                                                  */
/* ========================================================================== */

typedef unsigned __int8   uint8_t;
typedef unsigned __int16  uint16_t;
typedef unsigned __int32  uint32_t;
typedef unsigned __int64  uint64_t;
typedef signed   __int8   int8_t;
typedef signed   __int16  int16_t;
typedef signed   __int32  int32_t;
typedef signed   __int64  int64_t;

/* ========================================================================== */
/* Magic values                                                               */
/* ========================================================================== */

#define NX_MAGIC            0x4253584E  /* 'NXSB' */
#define APFS_MAGIC          0x42535041  /* 'APSB' */

/* ========================================================================== */
/* Object types                                                               */
/* ========================================================================== */

#define OBJECT_TYPE_NX_SUPERBLOCK       0x00000001
#define OBJECT_TYPE_BTREE               0x00000002
#define OBJECT_TYPE_BTREE_NODE          0x00000003
#define OBJECT_TYPE_OMAP                0x0000000B
#define OBJECT_TYPE_FS                  0x0000000D

/* ========================================================================== */
/* Object type masks and storage types                                        */
/* ========================================================================== */

#define OBJ_TYPE_MASK_TYPE      0x0000FFFF
#define OBJ_STORAGE_TYPE_MASK   0xC0000000
#define OBJ_PHYSICAL            0x00000000
#define OBJ_EPHEMERAL           0x80000000
#define OBJ_VIRTUAL             0x40000000

/* ========================================================================== */
/* B-tree node flags                                                          */
/* ========================================================================== */

#define BTNODE_ROOT             0x0001
#define BTNODE_LEAF             0x0002
#define BTNODE_FIXED_KV_SIZE    0x0004

/* ========================================================================== */
/* File-system record types                                                   */
/* ========================================================================== */

#define APFS_TYPE_SNAP_METADATA     1
#define APFS_TYPE_EXTENT            2
#define APFS_TYPE_INODE             3
#define APFS_TYPE_XATTR             4
#define APFS_TYPE_SIBLING_LINK      5
#define APFS_TYPE_DSTREAM_ID        6
#define APFS_TYPE_CRYPTO_STATE      7
#define APFS_TYPE_FILE_EXTENT       8
#define APFS_TYPE_DIR_REC           9
#define APFS_TYPE_DIR_STATS         10
#define APFS_TYPE_SNAP_NAME         11
#define APFS_TYPE_SIBLING_MAP       12

/* ========================================================================== */
/* Directory entry types                                                      */
/* ========================================================================== */

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

/* ========================================================================== */
/* Inode extended field types                                                  */
/* ========================================================================== */

#define INO_EXT_TYPE_SNAP_XID       1
#define INO_EXT_TYPE_DELTA_TREE_OID 2
#define INO_EXT_TYPE_DOCUMENT_ID    3
#define INO_EXT_TYPE_NAME           4
#define INO_EXT_TYPE_PREV_FSIZE     5
#define INO_EXT_TYPE_RESERVED_6     6
#define INO_EXT_TYPE_FINDER_INFO    7
#define INO_EXT_TYPE_DSTREAM        8

/* ========================================================================== */
/* FS record key masks                                                        */
/* ========================================================================== */

#define OBJ_ID_MASK     ((uint64_t)0x0FFFFFFF << 32 | (uint64_t)0xFFFFFFFF)
#define OBJ_TYPE_SHIFT  60
#define OBJ_TYPE_MASK   ((uint64_t)0xF0000000 << 32)

/* ========================================================================== */
/* Root directory inode                                                       */
/* ========================================================================== */

#define APFS_ROOT_DIR_INO  2

/* ========================================================================== */
/* File extent length mask                                                    */
/* ========================================================================== */

#define J_FILE_EXTENT_LEN_MASK  ((uint64_t)0x00FFFFFF << 32 | (uint64_t)0xFFFFFFFF)

/* ========================================================================== */
/* Directory record masks                                                     */
/* ========================================================================== */

#define J_DREC_LEN_MASK   0x000003ff
#define J_DREC_HASH_MASK  0xfffff400
#define J_DREC_HASH_SHIFT 10

#define DREC_TYPE_MASK    0x000f

/* ========================================================================== */
/* On-disk structures (all packed, little-endian)                              */
/* ========================================================================== */

#pragma pack(push, 1)

/* Object header -- common to all APFS objects */
typedef struct {
    uint64_t o_cksum;       /* Fletcher-64 checksum */
    uint64_t o_oid;         /* Object identifier */
    uint64_t o_xid;         /* Transaction identifier */
    uint32_t o_type;        /* Object type and flags */
    uint32_t o_subtype;     /* Object subtype */
} obj_phys_t;

/* Container Superblock */
typedef struct {
    obj_phys_t  nx_o;
    uint32_t    nx_magic;               /* 'NXSB' = 0x4253584E */
    uint32_t    nx_block_size;
    uint64_t    nx_block_count;
    uint64_t    nx_features;
    uint64_t    nx_readonly_compatible_features;
    uint64_t    nx_incompatible_features;
    uint8_t     nx_uuid[16];
    uint64_t    nx_next_oid;
    uint64_t    nx_next_xid;
    uint32_t    nx_xp_desc_blocks;
    uint32_t    nx_xp_data_blocks;
    uint64_t    nx_xp_desc_base;
    uint64_t    nx_xp_data_base;
    uint32_t    nx_xp_desc_next;
    uint32_t    nx_xp_data_next;
    uint32_t    nx_xp_desc_index;
    uint32_t    nx_xp_desc_len;
    uint32_t    nx_xp_data_index;
    uint32_t    nx_xp_data_len;
    uint64_t    nx_spaceman_oid;
    uint64_t    nx_omap_oid;
    uint64_t    nx_reaper_oid;
    uint32_t    nx_test_type;
    uint32_t    nx_max_file_systems;
    uint64_t    nx_fs_oid[100];         /* Volume OID array */
} nx_superblock_t;

/* Volume Superblock */
typedef struct {
    obj_phys_t  apfs_o;
    uint32_t    apfs_magic;             /* 'APSB' = 0x42535041 */
    uint32_t    apfs_fs_index;
    uint64_t    apfs_features;
    uint64_t    apfs_readonly_compatible_features;
    uint64_t    apfs_incompatible_features;
    uint64_t    apfs_unmount_time;
    uint64_t    apfs_fs_reserve_block_count;
    uint64_t    apfs_fs_quota_block_count;
    uint64_t    apfs_fs_alloc_count;
    /* wrapped_meta_crypto_state_t: 20 bytes */
    uint16_t    apfs_meta_crypto_major;
    uint16_t    apfs_meta_crypto_minor;
    uint32_t    apfs_meta_crypto_cpflags;
    uint32_t    apfs_meta_crypto_persistent_class;
    uint32_t    apfs_meta_crypto_key_os_version;
    uint16_t    apfs_meta_crypto_key_revision;
    uint16_t    apfs_meta_crypto_unused;
    uint32_t    apfs_root_tree_type;
    uint32_t    apfs_extentref_tree_type;
    uint32_t    apfs_snap_meta_tree_type;
    uint64_t    apfs_omap_oid;          /* Physical OID of volume omap */
    uint64_t    apfs_root_tree_oid;     /* Virtual OID of fs B-tree root */
    uint64_t    apfs_extentref_tree_oid;
    uint64_t    apfs_snap_meta_tree_oid;
    uint64_t    apfs_revert_to_xid;
    uint64_t    apfs_revert_to_sblock_oid;
    uint64_t    apfs_next_obj_id;
    uint64_t    apfs_num_files;
    uint64_t    apfs_num_directories;
    uint64_t    apfs_num_symlinks;
    uint64_t    apfs_num_other_fsobjects;
    uint64_t    apfs_num_snapshots;
    uint64_t    apfs_total_blocks_alloced;
    uint64_t    apfs_total_blocks_freed;
    uint8_t     apfs_vol_uuid[16];
    uint64_t    apfs_last_mod_time;
    uint64_t    apfs_fs_flags;
    uint8_t     apfs_formatted_by[32 + 8]; /* apfs_modified_by_t: id[32] + timestamp(8) + last_xid(8) = 48 bytes */
    uint8_t     apfs_modified_by[48 * 8]; /* array of 8 apfs_modified_by_t */
    char        apfs_volname[256];      /* Volume name (null-terminated UTF-8) */
    uint32_t    apfs_next_doc_id;
    uint16_t    apfs_role;
    uint16_t    apfs_reserved;
    uint64_t    apfs_root_to_xid;
    uint64_t    apfs_er_state_oid;
} apfs_superblock_t;

/* ========================================================================== */
/* B-Tree structures                                                          */
/* ========================================================================== */

/* Named location -- offset and length within a node */
typedef struct {
    uint16_t off;
    uint16_t len;
} nloc_t;

/* Variable-size key-value location (for variable-size KV nodes) */
typedef struct {
    nloc_t  k;      /* key offset and length */
    nloc_t  v;      /* value offset and length */
} kvloc_t;

/* Fixed-size key-value offset (for fixed-size KV nodes) */
typedef struct {
    uint16_t k;     /* key offset */
    uint16_t v;     /* value offset */
} kvoff_t;

/* B-tree node header */
typedef struct {
    obj_phys_t  btn_o;
    uint16_t    btn_flags;
    uint16_t    btn_level;          /* 0 = leaf */
    uint32_t    btn_nkeys;
    nloc_t      btn_table_space;
    nloc_t      btn_free_space;
    nloc_t      btn_key_free_list;
    nloc_t      btn_val_free_list;
    /* Followed by: TOC entries, keys area, values area */
} btree_node_phys_t;

/* B-tree info (fixed portion) */
typedef struct {
    uint32_t    bt_flags;
    uint32_t    bt_node_size;
    uint32_t    bt_key_size;
    uint32_t    bt_val_size;
} btree_info_fixed_t;

/* B-tree info (full, stored at end of root node) */
typedef struct {
    btree_info_fixed_t  bt_fixed;
    uint32_t            bt_longest_key;
    uint32_t            bt_longest_val;
    uint64_t            bt_key_count;
    uint64_t            bt_node_count;
} btree_info_t;

/* ========================================================================== */
/* Object Map structures                                                      */
/* ========================================================================== */

typedef struct {
    obj_phys_t  om_o;
    uint32_t    om_flags;
    uint32_t    om_snap_count;
    uint32_t    om_tree_type;
    uint32_t    om_snapshot_tree_type;
    uint64_t    om_tree_oid;        /* Physical OID of omap B-tree root */
    uint64_t    om_snapshot_tree_oid;
    uint64_t    om_most_recent_snap;
    uint64_t    om_pending_revert_min;
    uint64_t    om_pending_revert_max;
} omap_phys_t;

typedef struct {
    uint64_t ok_oid;
    uint64_t ok_xid;
} omap_key_t;

typedef struct {
    uint32_t ov_flags;
    uint32_t ov_size;
    uint64_t ov_paddr;      /* Physical block address */
} omap_val_t;

/* ========================================================================== */
/* File-system record structures                                              */
/* ========================================================================== */

/* Common key header for all FS records */
typedef struct {
    uint64_t obj_id_and_type;   /* Upper 4 bits: record type, lower 60 bits: OID */
} j_key_t;

/* Inode key */
typedef struct {
    j_key_t hdr;
} j_inode_key_t;

/* Inode value */
typedef struct {
    uint64_t parent_id;
    uint64_t private_id;
    uint64_t create_time;       /* nanoseconds since 1970-01-01 */
    uint64_t mod_time;
    uint64_t change_time;
    uint64_t access_time;
    uint64_t internal_flags;
    union {
        int32_t  nchildren;     /* directories */
        int32_t  nlink;         /* files */
    } nlink_or_nchildren;
    uint32_t default_protection_class;
    uint32_t write_generation_counter;
    uint32_t bsd_flags;
    uint32_t owner;
    uint32_t group;
    uint16_t mode;
    uint16_t pad1;
    uint64_t uncompressed_size;
    /* Followed by variable-length extended fields (xfields) */
} j_inode_val_t;

/* Directory entry key (hashed, used on case-insensitive volumes -- the default) */
typedef struct {
    j_key_t  hdr;
    uint32_t name_len_and_hash;
    /* Followed by name bytes (including null terminator) */
} j_drec_key_t;

/* Directory entry value */
typedef struct {
    uint64_t file_id;       /* inode number of the target */
    uint64_t date_added;
    uint16_t flags;         /* low 4 bits = d_type (DT_REG, DT_DIR, etc.) */
    /* Followed by variable-length extended fields (xfields) */
} j_drec_val_t;

/* File extent key */
typedef struct {
    j_key_t  hdr;           /* OID = private_id, type = APFS_TYPE_FILE_EXTENT */
    uint64_t logical_addr;  /* logical offset within the file */
} j_file_extent_key_t;

/* File extent value */
typedef struct {
    uint64_t len_and_flags;     /* lower 56 bits = length, upper 8 bits = flags */
    uint64_t phys_block_num;    /* physical block address */
    uint64_t crypto_id;
} j_file_extent_val_t;

/* Physical extent key */
typedef struct {
    j_key_t  hdr;           /* OID = physical block, type = APFS_TYPE_EXTENT */
    /* No additional fields beyond the key header */
} j_phys_ext_key_t;

/* Physical extent value */
typedef struct {
    uint64_t len_and_kind;  /* extent length and kind */
    uint64_t owning_obj_id; /* OID of the owning file-system object */
    int32_t  ref_count;     /* reference count */
} j_phys_ext_val_t;

/* Data stream (inode extended field) */
typedef struct {
    uint64_t size;
    uint64_t alloced_size;
    uint64_t default_crypto_id;
    uint64_t total_bytes_written;
    uint64_t total_bytes_read;
} j_dstream_t;

/* Inode extended field blob header (precedes the x_field_t array) */
typedef struct {
    uint16_t xf_num_exts;   /* number of extended fields */
    uint16_t xf_used_data;  /* total bytes used by field data */
} x_fields_hdr_t;

/* Inode extended field header */
typedef struct {
    uint8_t  x_type;        /* INO_EXT_TYPE_* */
    uint8_t  x_flags;       /* XF_* flags */
    uint16_t x_size;        /* size of the field data that follows */
} x_field_t;

#pragma pack(pop)

#endif /* APFS_TYPES_H */
