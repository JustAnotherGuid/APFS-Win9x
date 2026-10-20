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
 * apfs_main.c -- VxD initialization, IOS/IFSMgr registration
 *
 * Handles APFS_Dynamic_Init, APFS_Dynamic_Exit, the async event
 * routine (AER), and the mount thunk that IFSMgr calls when IOS
 * presents a new volume. Also contains the OurVolume detection
 * logic and the static volfunc table.
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* External declarations (defined in devdcl.asm)                              */
/* ========================================================================== */

extern DRP  theDRP;                /* defined in devdcl.asm (IDATA segment) */
extern void IosRegister(DRP* drp); /* defined in devdcl.asm (ICODE segment) */

/* ========================================================================== */
/* Forward declarations -- local functions                                    */
/* ========================================================================== */

static int  apfs_mount_thunk(pioreq pir);
static void apfs_async_event(PAEP aep);
static int  apfs_our_volume(apfs_volume_t* vol);
static void apfs_unmount_volume(apfs_volume_t* vol);
static void __cdecl apfs_our_volume_iocomplete(PIOR ior);

/* ========================================================================== */
/* Module-level state                                                         */
/* ========================================================================== */

static apfs_volume_t* g_volumes = NULL;     /* linked list of mounted volumes */
static int g_fsd_id = -1;                   /* IFSMgr FSD registration ID */
static int g_ios_registered = 0;            /* IOS registration flag */

static volfunc apfs_volfuncs = {
    IFS_VERSION,                        /* vfn_version */
    IFS_REVISION,                       /* vfn_revision */
    NUM_VOLFUNC,                        /* vfn_size */
    {
        (pIFSFunc) apfs_delete_file,         /* VFN_DELETE        = 0  */
        (pIFSFunc) apfs_dir,                 /* VFN_DIR           = 1  */
        (pIFSFunc) apfs_file_attributes,     /* VFN_FILEATTRIB    = 2  */
        (pIFSFunc) apfs_flush_volume,        /* VFN_FLUSH         = 3  */
        (pIFSFunc) apfs_get_disk_info,       /* VFN_GETDISKINFO   = 4  */
        (pIFSFunc) apfs_open_file,           /* VFN_OPEN          = 5  */
        (pIFSFunc) apfs_rename_file,         /* VFN_RENAME        = 6  */
        (pIFSFunc) apfs_search_file,         /* VFN_SEARCH        = 7  */
        (pIFSFunc) apfs_query_resource_info, /* VFN_QUERY         = 8  */
        (pIFSFunc) apfs_disconnect,	         /* VFN_DISCONNECT    = 9  */
        (pIFSFunc) apfs_empty_func,          /* VFN_UNCPIPEREQ    = 10 */
        (pIFSFunc) apfs_ioctl16_drive,       /* VFN_IOCTL16DRIVE  = 11 */
        (pIFSFunc) apfs_get_disk_parms,      /* VFN_GETDISKPARMS  = 12 */
        (pIFSFunc) apfs_find_first_file,     /* VFN_FINDOPEN      = 13 */
        (pIFSFunc) apfs_direct_disk_io       /* VFN_DASDIO        = 14 */
    }
};


/*
 * APFS_Dynamic_Init - Called by the VxD control procedure on SYS_DYNAMIC_DEVICE_INIT. 
 * Registers with IOS and IFSMgr, returns VXD_SUCCESS or VXD_FAILURE. This is standard
 * call as the control procedure in the assembly module calls it in that way.
 */

DWORD __stdcall APFS_Dynamic_Init(void) {
    DWORD ios_ver;
    DWORD ifs_ver;
    int fsd_id;

    Debug_Printf("APFS: Dynamic_Init starting\r\n");

    ios_ver = IOS_Get_Version();
    if (ios_ver == 0) {
        Debug_Printf("APFS: IOS not loaded, aborting\r\n");
        return VXD_FAILURE;
    }
    Debug_Printf("APFS: IOS version 0x%08lX\r\n", ios_ver);

    ifs_ver = IFSMgr_Get_Version();
    if (ifs_ver < IFSMGRVERSION) {
        Debug_Printf("APFS: IFSMgr version 0x%08lX too old (need >= 0x%02X)\r\n", ifs_ver, IFSMGRVERSION);
        return VXD_FAILURE;
    }
    Debug_Printf("APFS: IFSMgr version 0x%08lX\r\n", ifs_ver);


    theDRP.DRP_aer = apfs_async_event;
    IosRegister(&theDRP);
    g_ios_registered = 1;
    Debug_Printf("APFS: IOS_Register complete\r\n");

    fsd_id = IFSMgr_RegisterMount((pIFSFunc) apfs_mount_thunk, IFSMGRVERSION, NORMAL_FSD);
    if (fsd_id < 0) {
        Debug_Printf("APFS: IFSMgr_RegisterMount failed (%d)\r\n", fsd_id);
        return VXD_FAILURE;
    }
    g_fsd_id = fsd_id;
    Debug_Printf("APFS: IFSMgr_RegisterMount OK, FSD ID = %d\r\n", fsd_id);

    Debug_Printf("APFS: Dynamic_Init complete\r\n");
    return VXD_SUCCESS;
}


/*
 * APFS_Dynamic_Exit - Called by the VxD control procedure on SYS_DYNAMIC_DEVICE_EXIT. 
 * Unmounts drives and frees resources.
 */
DWORD __stdcall APFS_Dynamic_Exit(void) {
    apfs_volume_t* vol;
    apfs_volume_t* next;

    Debug_Printf("APFS: Dynamic_Exit starting\r\n");

    /* Walk the mounted volume list and unmount each one */
    vol = g_volumes;
    while (vol != NULL) {
        next = vol->next;
        apfs_unmount_volume(vol);
        vol = next;
    }
    g_volumes = NULL;

    Debug_Printf("APFS: Dynamic_Exit complete\r\n");
    return VXD_SUCCESS;
}


/*
 * apfs_async_event - Called by IOS for asynchronous events. We don't really 
 * do much here as the main purpose of this VXD is to be an installabe File-
 * System driver and not really be a port driver. So we just return AEP_SUCCESS.
 */
static void apfs_async_event(PAEP aep) {
    USHORT func;

    func = aep->AEP_func;

    switch (func) {
    case AEP_INITIALIZE:
        Debug_Printf("APFS: AEP_INITIALIZE received\r\n");
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_BOOT_COMPLETE:
        Debug_Printf("APFS: AEP_BOOT_COMPLETE received\r\n");
        aep->AEP_result = AEP_SUCCESS;
        break;

    default:
        /* Return success for all other events */
        aep->AEP_result = AEP_SUCCESS;
        break;
    }
}


/*
 * apfs_our_volume_iocomplete -- IOR completion callback for mount-time reads.
 *
 * Sets the completion flag and wakes the blocked thread.
 * Same pattern as LPFS OnCommandComplete.
 */
static void __cdecl apfs_our_volume_iocomplete(PIOR ior) {
    ior->IOR_completion_flag = 1;
    IFSMgr_Wakeup((DWORD) &ior->IOR_completion_flag);
}

/*
 * our_volume_read_sector -- Read a single sector synchronously during mount.
 *
 */
static int our_volume_read_sector(apfs_volume_t* vol, DWORD sector_num,
                                   uint8_t* buf) {
    PVRP vrp;
    PDCB dcb;
    PIOR ior;
    USHORT iop_size;
    ULONG delta;
    DWORD status;

    vrp = vol->vrp;
    dcb = (PDCB) vrp->VRP_device_handle;

    iop_size = vrp->VRP_max_req_size + vrp->VRP_max_sgd * sizeof(SGD);
    delta = vrp->VRP_delta_to_ior;

    ior = IspCreateIor(iop_size, delta, 0);
    if (ior == NULL) {
        return -1;
    }

    ior->IOR_next            = 0;
    ior->IOR_func            = IOR_READ;
    ior->IOR_flags           = IORF_VERSION_002 | IORF_BYPASS_VOLTRK
                               | IORF_HIGH_PRIORITY;
    ior->IOR_start_addr[0]   = sector_num;
    ior->IOR_start_addr[1]   = 0;
    ior->IOR_xfer_count      = 1;
    ior->IOR_buffer_ptr      = (ULONG) buf;
    ior->IOR_num_sgds        = 0;
    ior->IOR_vol_designtr    = vol->drive;
    ior->IOR_req_vol_handle  = (ULONG) vrp;
    ior->IOR_private_client  = delta;
    ior->IOR_sgd_lin_phys    = (ULONG)(ior + 1);

    if (vrp->VRP_demand_flags & VRP_dmd_removable_supp) {
        ior->IOR_flags &= ~IORF_LOGICAL_START_SECTOR;
        ior->IOR_start_addr[0] += vrp->VRP_partition_offset;
    } else {
        ior->IOR_flags |= IORF_LOGICAL_START_SECTOR;
    }

    ior->IOR_completion_flag = 0;
    ior->IOR_callback        = (CMDCPLT) apfs_our_volume_iocomplete;
    ior->IOR_req_req_handle  = (ULONG) ior;

    /* IOS may modify EDI */
    _asm push edi
    IOS_SendCommand(ior, dcb);
    _asm pop edi

    /* Note the DDK and LPFS (Systems Programming for Windows 95)
	 * wrap these waits with cli/sti commands however IFSMgr_Block
	 * itself is just a wrapper around __BlockOnId with interrupts enabled.
	 */
    while (!ior->IOR_completion_flag) {
        IFSMgr_Block((DWORD) &ior->IOR_completion_flag);
    }

    status = ior->IOR_status;
    IspDeallocMem((PVOID)((DWORD)ior - ior->IOR_private_client));

    if (status >= IORS_ERROR_DESIGNTR) {
        return (int) IOSMapIORSToI21(status);
    }
    return 0;
}

/*
 * apfs_our_volume - Reads sector 0 (protective MBR), checks for GPT,  
 * scans partition entries for the APFS GUID, reads the container 
 * superblock, and verifies NX_MAGIC.
 * Returns 0 if this is an APFS volume, -1 if not.
 */
static int apfs_our_volume(apfs_volume_t* vol) {
    uint8_t* sector_buf;
    gpt_header_t* gpt_hdr;
    uint8_t* entry_buf;
    uint32_t i;
    uint32_t num_entries;
    uint32_t entry_size;
    uint64_t entry_lba;
    uint64_t partition_start;
    int found;
    nx_superblock_t* nx_sb;
    uint16_t mbr_sig;
    uint8_t part_type;
    int rc;
    uint32_t entries_per_sector;
    uint32_t sector_index;
    uint32_t offset_in_sector;
    uint32_t last_sector_read;

    sector_buf = (uint8_t*) _HeapAllocate(SECTOR_SIZE, HEAPZEROINIT);
    if (sector_buf == NULL) {
        Debug_Printf("APFS: OurVolume: out of memory for sector buffer\r\n");
        return -1;
    }

    rc = our_volume_read_sector(vol, 0, sector_buf);
    if (rc != 0) {
        Debug_Printf("APFS: OurVolume: sector 0 read failed (rc=%d)\r\n", rc);
        _HeapFree(sector_buf, 0);
        return -1;
    }

    /* Check MBR signature 0x55AA at offset 510 */
    mbr_sig = *(uint16_t*)(sector_buf + MBR_SIG_OFFSET);
    if (mbr_sig != MBR_SIGNATURE) {
        Debug_Printf("APFS: OurVolume: no MBR signature (got 0x%04X)\r\n",
                      mbr_sig);
        _HeapFree(sector_buf, 0);
        return -1;
    }

    /* Check first partition entry type is 0xEE (GPT protective) */
    part_type = sector_buf[MBR_PART_TABLE_OFF + MBR_PART_TYPE_OFF];
    if (part_type != MBR_PART_TYPE_GPT) {
        Debug_Printf("APFS: OurVolume: not GPT (partition type 0x%02X)\r\n",
                      part_type);
        _HeapFree(sector_buf, 0);
        return -1;
    }


    memset(sector_buf, 0, SECTOR_SIZE);
    rc = our_volume_read_sector(vol, 1, sector_buf);
    if (rc != 0) {
        Debug_Printf("APFS: OurVolume: GPT header read failed\r\n");
        _HeapFree(sector_buf, 0);
        return -1;
    }

    /* Verify "EFI PART" signature */
    gpt_hdr = (gpt_header_t*) sector_buf;
    if (gpt_hdr->signature != GPT_SIGNATURE) {
        Debug_Printf("APFS: OurVolume: GPT signature mismatch\r\n");
        _HeapFree(sector_buf, 0);
        return -1;
    }

    num_entries = gpt_hdr->num_partition_entries;
    entry_size  = gpt_hdr->size_of_partition_entry;
    entry_lba   = gpt_hdr->partition_entry_lba;

    Debug_Printf("APFS: OurVolume: GPT found, %lu entries at LBA %lu\r\n",
                  (DWORD) num_entries, (DWORD) entry_lba);

    _HeapFree(sector_buf, 0);
    sector_buf = NULL;


    if (entry_size < sizeof(gpt_partition_entry_t)) {
        entry_size = sizeof(gpt_partition_entry_t);
    }

    entry_buf = (uint8_t*) _HeapAllocate(SECTOR_SIZE, HEAPZEROINIT);
    if (entry_buf == NULL) {
        Debug_Printf("APFS: OurVolume: out of memory for partition entries\r\n");
        return -1;
    }

    found = 0;
    partition_start = 0;
    entries_per_sector = SECTOR_SIZE / entry_size;
    if (entries_per_sector == 0) entries_per_sector = 1;
    last_sector_read = 0xFFFFFFFF;

    for (i = 0; i < num_entries && !found; i++) {
        gpt_partition_entry_t* entry;
        uint32_t target_sector;

        sector_index = i / entries_per_sector;
        offset_in_sector = (i % entries_per_sector) * entry_size;
        target_sector = (uint32_t)(entry_lba + sector_index);

        /* Read the sector containing this entry if not already read */
        if (target_sector != last_sector_read) {
            memset(entry_buf, 0, SECTOR_SIZE);
            rc = our_volume_read_sector(vol, target_sector, entry_buf);
            if (rc != 0) {
                Debug_Printf("APFS: OurVolume: GPT entry read failed\r\n");
                _HeapFree(entry_buf, 0);
                return -1;
            }
            last_sector_read = target_sector;
        }

        entry = (gpt_partition_entry_t*)(entry_buf + offset_in_sector);

        if (memcmp(entry->partition_type_guid, APFS_GPT_GUID, 16) == 0) {
            partition_start = entry->starting_lba * SECTOR_SIZE;
            Debug_Printf("APFS: OurVolume: APFS partition found at LBA %lu\r\n",
                          (DWORD) entry->starting_lba);
            found = 1; /* Break here??*/
        }
    }

    _HeapFree(entry_buf, 0);
    if (!found) {
        Debug_Printf("APFS: OurVolume: no APFS partition in GPT\r\n");
        return -1;
    }

    vol->partition_offset = partition_start;
    sector_buf = (uint8_t*) _HeapAllocate(SECTOR_SIZE, HEAPZEROINIT);
    if (sector_buf == NULL) {
        Debug_Printf("APFS: OurVolume: out of memory for superblock check\r\n");
        return -1;
    }

    rc = our_volume_read_sector(vol, (DWORD)(partition_start / SECTOR_SIZE), sector_buf);
    if (rc != 0) {
        Debug_Printf("APFS: OurVolume: superblock read failed\r\n");
        _HeapFree(sector_buf, 0);
        return -1;
    }

    nx_sb = (nx_superblock_t*) sector_buf;
    if (nx_sb->nx_magic != NX_MAGIC) {
        Debug_Printf("APFS: OurVolume: NX_MAGIC mismatch (got 0x%08lX)\r\n",
                      (DWORD) nx_sb->nx_magic);
        _HeapFree(sector_buf, 0);
        return -1;
    }

    Debug_Printf("APFS: OurVolume: NX_MAGIC verified, block_size=%lu\r\n",
                  (DWORD) nx_sb->nx_block_size);
    vol->block_size = nx_sb->nx_block_size;
    _HeapFree(sector_buf, 0);
    return 0;
}



/*
 * apfs_unmount_volume - Unmounts passed volume.
 */
static void apfs_unmount_volume(apfs_volume_t* vol) {
    if (vol == NULL) return;

    Debug_Printf("APFS: unmounting volume on drive %c:\r\n",
                  'A' + vol->drive);

    /* Invalidate and free the block cache */
    if (vol->cache != NULL) {
        cache_invalidate_all(vol->cache);
        cache_destroy(vol->cache);
        vol->cache = NULL;
    }

    /* Remove from linked list */
    if (vol->prev != NULL) {
        vol->prev->next = vol->next;
    } else {
        g_volumes = vol->next;
    }
    if (vol->next != NULL) {
        vol->next->prev = vol->prev;
    }

    _HeapFree(vol, 0);
}

/*
 * apfs_mount_thunk - Called by IFSMgr for every new volume. 
 * We determine if the volume is APFS and if so, claim it.    
 */
static int apfs_mount_thunk(pioreq pir) {
    int flags;

    flags = pir->ir_flags;

    switch (flags) {

    case IR_FSD_MOUNT: {
        apfs_volume_t* vol;
        PVRP vrp;
        int rc;

        Debug_Printf("APFS: IR_FSD_MOUNT on drive %d\r\n", pir->ir_mntdrv);

        vol = (apfs_volume_t*) _HeapAllocate(sizeof(apfs_volume_t), HEAPZEROINIT);
        if (vol == NULL) {
            Debug_Printf("APFS: mount: out of memory for volume\r\n");
			pir->ir_error = ERROR_NOT_ENOUGH_MEMORY;
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        vrp = (PVRP) pir->ir_volh;
        vol->vrp = vrp;
        vol->drive = (BYTE) pir->ir_mntdrv;
        vol->orig_drive = vol->drive;

        rc = apfs_our_volume(vol);
        if (rc != 0) {
            Debug_Printf("APFS: mount: not an APFS volume\r\n");
            _HeapFree(vol, 0);
			pir->ir_error = ERROR_UNRECOGNIZED_VOLUME;
            return ERROR_UNRECOGNIZED_VOLUME;
        }

        rc = apfs_container_open(vol);
        if (rc != 0) {
            Debug_Printf("APFS: mount: container open failed\r\n");
            _HeapFree(vol, 0);
			pir->ir_error = ERROR_UNRECOGNIZED_VOLUME;
            return ERROR_UNRECOGNIZED_VOLUME;
        }

        rc = apfs_volume_open(vol, 0);
        if (rc != 0) {
            Debug_Printf("APFS: mount: volume open failed\r\n");
            _HeapFree(vol, 0);
			pir->ir_error = ERROR_UNRECOGNIZED_VOLUME;
            return ERROR_UNRECOGNIZED_VOLUME;
        }

        vol->cache = cache_init(vol->block_size);
        if (vol->cache == NULL) {
            Debug_Printf("APFS: mount: cache init failed\r\n");
            _HeapFree(vol, 0);
			pir->ir_error = ERROR_NOT_ENOUGH_MEMORY;
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        pir->ir_rh    = (rh_t) vol;
        pir->ir_vfunc = &apfs_volfuncs;

        vrp->VRP_fsd_hvol = (ULONG) vol;
		vol->next = g_volumes;
        vol->prev = NULL;
        if (g_volumes != NULL) {
            g_volumes->prev = vol;
        }
        g_volumes = vol;

        Debug_Printf("APFS: mount: volume '%s' mounted on drive %c:\r\n", vol->vol_label, 'A' + vol->drive);
		pir->ir_error = 0;
        return 0;
    }

    case IR_FSD_VERIFY: {
        apfs_volume_t* vol;
        vol = (apfs_volume_t*) pir->ir_rh;
        if (vol == NULL) {
            return -1;
        }
        Debug_Printf("APFS: IR_FSD_VERIFY on drive %c:\r\n", 'A' + vol->drive);
        /* TODO: Actually do something here */
		pir->ir_error = 0;
        return 0;
    }

    case IR_FSD_UNLOAD: {
        apfs_volume_t* vol;
        vol = (apfs_volume_t*) pir->ir_rh;
        Debug_Printf("APFS: IR_FSD_UNLOAD on drive %c:\r\n", vol ? ('A' + vol->drive) : '?');
        if (vol != NULL) {
            apfs_unmount_volume(vol);
        }
		pir->ir_error = 0;
        return 0;
    }

    case IR_FSD_MOUNT_CHILD:
        Debug_Printf("APFS: IR_FSD_MOUNT_CHILD rejected\r\n");
		pir->ir_error = ERROR_ACCESS_DENIED;
        return ERROR_ACCESS_DENIED;

    case IR_FSD_MAP_DRIVE: {
        apfs_volume_t* vol;

        vol = (apfs_volume_t*) pir->ir_rh;
        if (vol != NULL) {
            vol->drive = (BYTE) pir->ir_options;
            Debug_Printf("APFS: IR_FSD_MAP_DRIVE -> %c:\r\n", 'A' + vol->drive);
        }
		pir->ir_error = 0;
        return 0;
    }

    case IR_FSD_UNMAP_DRIVE: {
        apfs_volume_t* vol;

        vol = (apfs_volume_t*) pir->ir_rh;
        if (vol != NULL) {
            vol->drive = vol->orig_drive;
            Debug_Printf("APFS: IR_FSD_UNMAP_DRIVE -> %c:\r\n", 'A' + vol->drive);
        }
		pir->ir_error = 0;
        return 0;
    }

    default:
        Debug_Printf("APFS: mount_thunk: unknown flags 0x%08lX\r\n", (DWORD) flags);
		pir->ir_error = ERROR_ACCESS_DENIED;
        return ERROR_ACCESS_DENIED;
    }
}
