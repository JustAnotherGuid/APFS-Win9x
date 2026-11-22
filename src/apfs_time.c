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
 * apfs_time.c -- APFS timestamp to DOS time conversion
 *
 * Converts APFS nanosecond timestamps (relative to 1970-01-01)
 * to DOS date/time format via IFSMgr_Win32ToDosTime.
 *
 * Conversion chain:
 *   1. APFS nanoseconds since 1970-01-01 -> seconds since 1970-01-01
 *   2. + 946684800 -> Unix seconds since 1970-01-01
 *   3. x 10000000 -> Win32 FILETIME (100-nanosecond intervals)
 *   4. IFSMgr_Win32ToDosTime() -> packed DOS date/time
 *
 */

#include "apfs_vxd.h"

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

/*
 * Win32 epoch offset: seconds from Win32 FILETIME epoch (1601-01-01)
 * to Unix epoch (1970-01-01).
 */
#define WIN32_EPOCH_OFFSET  ((uint64_t)11644473600)

/*
 * Number of 100-nanosecond intervals per second (FILETIME units).
 */
#define FILETIME_UNITS_PER_SEC  ((uint64_t)10000000)

/*
 * One billion -- nanoseconds per second.
 */
#define NANOSECONDS_PER_SEC ((uint64_t)1000000000)

/*
 * DOS epoch minimum: 1980-01-01 00:00:00.
 *
 * DOS date format: bits 15-9 = year (offset from 1980), bits 8-5 = month,
 *                  bits 4-0 = day.
 * DOS time format: bits 15-11 = hours, bits 10-5 = minutes,
 *                  bits 4-0 = seconds/2.
 *
 * 1980-01-01 00:00:00 -> date = (0<<9)|(1<<5)|1 = 0x0021, time = 0x0000.
 *
 * dos_time is returned as a DWORD: high word = date, low word = time.
 */
#define DOS_EPOCH_DATE  0x0021
#define DOS_EPOCH_TIME  0x0000

/* ========================================================================== */
/* Static helpers                                                             */
/* ========================================================================== */

/*
 * make_dos_time -- Pack a DOS date and time into a dos_time value.
 *
 * The DDK's dos_time is a DWORD with the date in the high 16 bits
 * and the time in the low 16 bits.
 */
static dos_time make_dos_time(uint16_t date, uint16_t time_val) {
	dos_time dt;

	dt.dt_time = time_val;
	dt.dt_date = date;
	return dt;
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/*
 * apfs_to_dos_time -- Convert APFS nanosecond timestamp to DOS time format.
 *
 * apfs_ns:  APFS timestamp (nanoseconds since 1970-01-01 00:00:00 UTC)
 * Returns:  dos_time with packed date and time fields
 */
dos_time apfs_to_dos_time(uint64_t apfs_ns) {
	uint64_t secs;
	uint64_t win32_100ns;
	_FILETIME ft;


	if (apfs_ns == 0) {
		return make_dos_time(DOS_EPOCH_DATE, DOS_EPOCH_TIME);
	}

	secs = (apfs_ns / NANOSECONDS_PER_SEC);
	win32_100ns = ((uint64_t)secs + WIN32_EPOCH_OFFSET) * FILETIME_UNITS_PER_SEC;

	ft.dwLowDateTime  = (uint32_t)win32_100ns;
	ft.dwHighDateTime  = (uint32_t)(win32_100ns >> 32);

	return IFSMgr_Win32ToDosTime(ft);
}
