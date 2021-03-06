ion.  This is a variable-length array of LcnsToFollow
    //  entries, only the first of which is declared.  Note that the writer
    //  always writes pages according to the physical page size on his
    //  machine, however whenever the log file is being read, no assumption
    //  is made about page size.  This is to facilitate moving disks between
    //  systems with different page sizes.
    //

    LCN LcnsForPage[1];

} DIRTY_PAGE_ENTRY_V0, *PDIRTY_PAGE_ENTRY_V0;

#pragma pack()

//
//  Dirty Pages Table - Version 1
//
//  This version correctly aligns the 64-bit fields.
//
//  One entry exists in the Dirty Pages Table for each page which is
//  dirty at the time the Restart Area is written.
//
//  This table is initialized at Restart to the maximum of
//  DEFAULT_DIRTY_PAGES_TABLE_SIZE or the size of the table in the log file.
//  It is *not* maintained in the running system.
//

typedef struct _DIRTY_PAGE_ENTRY {

    //
    //  Entry is allocated if this field contains RESTART_ENTRY_ALLOCATED.
    //  Otherwise, it is a free link.
    //

    ULONG AllocatedOrNextFree;

    //
    //  Target attribute index.  This is the index into the Open Attribute
    //  Table to which this dirty page entry applies.
    //

    ULONG TargetAttribute;

    //
    //  Length of transfer, in case this is the end of file, and we cannot
    //  write an entire page.
    //

    ULONG LengthOfTransfer;

    //
    //  Number of Lcns in the array at end of this structure.  See comment
    //  with this array.
    //

    ULONG LcnsToFollow;

    //
    //  Vcn of dirty page.
    //

    VCN Vcn;

    //
    //  OldestLsn for log record for which the update has not yet been
    //  written through to disk.
    //

    LSN OldestLsn;

    //
    //  Run information.  This is a variable-length array of LcnsToFollow
    //  entries, only the first of which is declared.  Note that the writer
    //  always writes pages according to the physical page size on his
    //  machine, however whenever the log file is being read, no assumption
    //  is made about page size.  This is to facilitate moving disks between
    //  systems with different page sizes.
    //

    LCN LcnsForPage[1];

} DIRTY_PAGE_ENTRY, *PDIRTY_PAGE_ENTRY;

//
//  Transaction Table
//
//  One transaction entry exists for each existing transaction at the time
//  the Restart Area is written.
//
//  Currently only local transactions are supported, and the transaction
//  ID is simply used to index into this table.
//
//  This table is initialized at Restart to the maximum of
//  DEFAULT_TRANSACTION_TABLE_SIZE or the size of the table in the log file.
//  It is maintained in the running system.
//

typedef struct _TRANSACTION_ENTRY {

    //
    //  Entry is allocated if this field contains RESTART_ENTRY_ALLOCATED.
    //  Otherwise, it is a free link.
    //

    ULONG AllocatedOrNextFree;

    //
    //  Transaction State
    //

    UCHAR TransactionState;

    //
    //  Reserved for proper alignment
    //

    UCHAR Reserved[3];

    //
    //  First Lsn for transaction.  This tells us how far back in the log
    //  we may have to read to abort the transaction.
    //

    LSN FirstLsn;

    //
    //  PreviousLsn written for the transaction and UndoNextLsn (next record
    //  which should be undone in the event of a rollback.
    //

    LSN PreviousLsn;
    LSN UndoNextLsn;

    //
    //  Number of of undo log records pending abort, and total undo size.
    //

    ULONG UndoRecords;
    LONG UndoBytes;

} TRANSACTION_ENTRY, *PTRANSACTION_ENTRY;

//
//  Restart record
//
//  The Restart record used by NTFS is small, and it only describes where
//  the above information has been written to the log.  The above records
//  may be considered logically part of NTFS's restart area.
//

typedef struct _RESTART_AREA {

    //
    //  Version numbers of NTFS Restart Implementation
    //

    ULONG MajorVersion;
    ULONG MinorVersion;

    //
    //  Lsn of Start of Checkpoint.  This is the Lsn at which the Analysis
    //  Phase of Restart must begin.
    //

    LSN StartOfCheckpoint;

    //
    //  Lsns at which the four tables above plus the attribute names reside.
    //

    LSN OpenAttributeTableLsn;
    LSN AttributeNamesLsn;
    LSN DirtyPageTableLsn;
    LSN TransactionTableLsn;

    //
    //  Lengths of the above structures in bytes.
    //

    ULONG OpenAttributeTableLength;
    ULONG AttributeNamesLength;
    ULONG DirtyPageTableLength;
    ULONG TransactionTableLength;

    //
    //  Oldest Usn from which scan must occur to pickup all files which
    //  have not been through cleanup.
    //

    USN LowestOpenUsn;

    LSN CurrentLsnAtMount;
    ULONG BytesPerCluster;

    ULONG Reserved;

    //
    //  Keep some additional information about the Usn journal so we
    //  can reduce the amount of caching we do.
    //

    FILE_REFERENCE UsnJournalReference;
    LONGLONG UsnCacheBias;

} RESTART_AREA, *PRESTART_AREA;

//
//  This symbols is used to accept Restart Areas with or without the OldestUsn
//

#define SIZEOF_OLD_RESTART_AREA          (FIELD_OFFSET( RESTART_AREA, LowestOpenUsn ))


//
//  RECORD STRUCTURES USED BY LOG RECORDS
//

//
//  Set new attribute sizes
//

typedef struct _NEW_ATTRIBUTE_SIZES {

    LONGLONG AllocationSize;
    LONGLONG ValidDataLength;
    LONGLONG FileSize;
    LONGLONG TotalAllocated;

} NEW_ATTRIBUTE_SIZES, *PNEW_ATTRIBUTE_SIZES;

#define SIZEOF_FULL_ATTRIBUTE_SIZES (                   \
    sizeof( NEW_ATTRIBUTE_SIZES )                       \
)

#define SIZEOF_PARTIAL_ATTRIBUTE_SIZES (                \
    FIELD_OFFSET( NEW_ATTRIBUTE_SIZES, TotalAllocated ) \
)

//
//  Describe a bitmap range
//

typedef struct _BITMAP_RANGE {

    ULONG BitMapOffset;
    ULONG NumberOfBits;

} BITMAP_RANGE, *PBITMAP_RANGE;

//
//  Describe a range of Lcns
//

typedef struct _LCN_RANGE {

    LCN StartLcn;
    LONGLONG Count;

} LCN_RANGE, *PLCN_RANGE;

#endif //  _NTFSLOG_
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             /*++

Copyright (c) 1991  Microsoft Corporation

Module Name:

    NtfsProc.h

Abstract:

    This module defines all of the globally used procedures in the Ntfs
    file system.

Author:

    Brian Andrew    [BrianAn]       21-May-1991
    David Goebel    [DavidGoe]
    Gary Kimura     [GaryKi]
    Tom Miller      [TomM]

Revision History:

--*/

#ifndef _NTFSPROC_
#define _NTFSPROC_


#if DBG
#define __NDAS_NTFS_DBG__						1
#endif

#define __NDAS_NTFS__							1
#define __NDAS_NTFS_DISABLE_REINIT__			1

#define __NDAS_NTFS_WIN2K_SUPPORT__				0

#define __NDAS_NTFS_PRIMARY__					1
#define __NDAS_NTFS_SECONDARY__					1
#define __NDAS_NTFS_DIRECT_READWRITE__			1
#define __NDAS_NTFS_DIRECT_CLEANUP__			1
#define __NDAS_NTFS_PURGE__						1
#define __NDAS_NTFS_FLUSH__						0

#define __NDAS_NTFS_DISABLE_SPARSE__			1

#define __NDAS_NTFS_AVOID_BUG__					1

#define __NDAS_NTFS_RECOVERY_TEST__				1

#define __NDAS_NTFS_VIEW_INDEX_DONT_SURPORT__	0
#define __NDAS_NTFS_DISABLE_ASSERT_DBG__		0

#define __NDAS_NTFS_PRIMARY_DISMOUNT__			0

#define __NDAS_NTFS_AVOID_LOG__					1

#if __NDAS_NTFS_WIN2K_SUPPORT__
#define _WIN2K_COMPAT_SLIST_USAGE	1
#endif

#pragma warning(error:4100)   // Unreferenced formal parameter
#pragma warning(error:4101)   // Unreferenced local variable
#pragma warning(error:4705)   // Statement has no effect
#pragma warning(disable:4116) // unnamed type definition in parentheses

#if !__NDAS_NTFS_WIN2K_SUPPORT__
#define RTL_USE_AVL_TABLES 0
#endif

#ifndef KDEXTMODE

#include <ntifs.h>

#else

#include <ntos.h>
#include <zwapi.h>
#include <FsRtl.h>
#include <ntrtl.h>

#endif


#if __NDAS_NTFS__

#include <NdasFs.h>
#include <SocketLpx.h>
#include <LpxTdi.h>
#include <NdfsProtocolHeader2.h>
#include <ntdddisk.h>
#include <ndasbusioctl.h>
#include <NdfsInteface.h>

#if __NDAS_NTFS_SECONDARY__

typedef enum _TYPE_OF_OPEN {

	UnopenedFileObject = 1,
	UserFileOpen,
	UserDirectoryOpen,
	UserVolumeOpen,
	StreamFileOpen,
	UserViewIndexOpen

} TYPE_OF_OPEN;

#endif

#endif

#include <string.h>
#include <lfs.h>
#include <ntdddisk.h>
#include <NtIoLogc.h>
#include <elfmsg.h>

#include "nodetype.h"
#include "Ntfs.h"

#ifndef INLINE
// definition of inline
#define INLINE __inline
#endif

#include <ntfsexp.h>

#include "NtfsStru.h"
#include "NtfsData.h"
#include "NtfsLog.h"


#if __NDAS_NTFS__

#include "md5.h"
#if __NDAS_NTFS_DISABLE_ASSERT_DBG__

#undef ASSERT
#define ASSERT( a )

#endif

#include "ndasntfs.h"

#if __NDAS_NTFS_PRIMARY__

#include "Primary.h"

#endif

#if __NDAS_NTFS_SECONDARY__

#include "Secondary.h"

#define	INITIALIZE_NDFS_WINXP_REQUEST_HEADER(	\
				MndfsWinxpRequestHeader,		\
				Mirp,							\
				MirpSp,							\
				MprimaryFileHandle				\
				);								\
{																			\
	/*(MndfsWinxpRequestHeader)->IrpTag			= (_U32)(Mirp);*/			\
	(MndfsWinxpRequestHeader)->IrpMajorFunction = (MirpSp)->MajorFunction;	\
	(MndfsWinxpRequestHeader)->IrpMinorFunction = (MirpSp)->MinorFunction;	\
	(MndfsWinxpRequestHeader)->FileHandle		= (MprimaryFileHandle);		\
	(MndfsWinxpRequestHeader)->IrpFlags			= (Mirp)->Flags;			\
	(MndfsWinxpRequestHeader)->IrpSpFlags		= (MirpSp)->Flags;			\
}

#endif


#if WINVER >= 0x0501
#define NdasNtfsDbgBreakPoint()	(KD_DEBUGGER_ENABLED ? DbgBreakPoint() : TRUE)
#else
#define NdasNtfsDbgBreakPoint()	((*KdDebuggerEnabled) ? DbgBreakPoint() : TRUE) 
#endif

#if DBG

#define NDASNTFS_ASSERT( exp )	ASSERT( exp )

#else

#define NDASNTFS_ASSERT( exp )	\
	((!(exp)) ?					\
	NdasNtfsDbgBreakPoint() :	\
	TRUE)

#endif

#define DebugTrace2(INDENT, LEVEL, M) DebugTrace(INDENT,LEVEL,M)

#endif


//
//  Tag all of our allocations if tagging is turned on
//

//
//  Default module pool t