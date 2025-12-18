/** @file
  SDP RAID driver private data structures and definitions.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef _SDP_RAID_H_
#define _SDP_RAID_H_

#include <Uefi.h>

#include <IndustryStandard/Pci.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/PciIo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// Superblock magic number: "SDPM" (SDP Metadata)
//
#define SDP_SUPERBLOCK_MAGIC  0x5344504D

//
// Superblock version
//
#define SDP_SUPERBLOCK_VERSION  1

//
// RAID levels
//
#define SDP_RAID_LEVEL_0  0  // Striping
#define SDP_RAID_LEVEL_1  1  // Mirroring
#define SDP_RAID_LEVEL_5  5  // Parity

//
// Maximum number of members in a RAID array
//
#define SDP_MAX_MEMBERS  16

//
// Superblock structure (512 bytes, stored at last LBA of each member disk)
// This is the on-disk metadata format
//
#pragma pack(1)
typedef struct {
  //
  // Magic number and version (8 bytes)
  //
  UINT32    Magic;           // 0x5344504D ("SDPM")
  UINT32    Version;         // 1

  //
  // Array identification (16 bytes)
  //
  EFI_GUID  ArrayUuid;       // Unique ID for this RAID array

  //
  // RAID configuration (12 bytes)
  //
  UINT8     RaidLevel;       // 0 = RAID0, 1 = RAID1, 5 = RAID5
  UINT8     NumMembers;      // Total number of disks in array
  UINT8     MemberIndex;     // This disk's index (0-based)
  UINT8     Reserved1;

  UINT64    ArraySizeInBlocks;     // Total size of RAID array in LBAs
  UINT64    MemberOffsetInBlocks;  // Starting offset on each member (usually 0)

  //
  // Stripe configuration (8 bytes)
  //
  UINT32    StripeSize;      // Stripe size in blocks (e.g., 128 = 64KB for 512-byte blocks)
  UINT32    Checksum;        // Simple checksum for validation

  //
  // Reserved for future use (464 bytes)
  // Total structure size = 8 + 16 + 12 + 16 + 8 + 464 = 524 bytes
  // Adjusted to fit in 512 bytes: 8 + 16 + 4 + 16 + 8 + 460 = 512 bytes
  //
  UINT8     Reserved2[460];
} SDP_SUPERBLOCK;
#pragma pack()

//
// Compile-time assertion to ensure superblock is exactly 512 bytes
//
STATIC_ASSERT (
  sizeof (SDP_SUPERBLOCK) == 512,
  "SDP_SUPERBLOCK must be exactly 512 bytes"
  );

//
// Member disk structure (runtime data for each physical disk in a RAID array)
// NOTE: For assembled arrays, we store Handle and get BlockIo via OpenProtocol when needed
//
typedef struct {
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;       // BlockIO protocol of physical disk (cached after assembly)
  EFI_HANDLE             Handle;         // Handle of physical disk
  UINT8                  MemberIndex;    // Index in array (from superblock)
  BOOLEAN                Present;        // TRUE if disk is present and healthy
  SDP_SUPERBLOCK         Superblock;     // Copy of superblock from this disk
} SDP_MEMBER_DISK;

//
// Member info for pending arrays (Phase 4: event-based discovery)
// Store Handle, NOT BlockIo pointer - protocols can be reinstalled
//
typedef struct {
  EFI_HANDLE      Handle;         // Handle of physical disk
  UINT8           MemberIndex;    // Index in array (from superblock)
  BOOLEAN         Present;        // TRUE if this member slot is filled
  SDP_SUPERBLOCK  Superblock;     // Copy of superblock from this disk
} SDP_MEMBER_INFO;

//
// Pending RAID array (tracking members as they're discovered)
// This structure is used during event-based discovery before assembly
//
typedef struct {
  LIST_ENTRY       Link;                      // Link in global pending list
  EFI_GUID         ArrayUuid;                 // UUID from superblock
  UINT8            RaidLevel;                 // RAID level
  UINT8            NumMembers;                // Expected total members
  UINT32           StripeSize;                // Stripe size (for consistency check)
  UINT8            FoundCount;                // Members discovered so far
  SDP_MEMBER_INFO  Members[SDP_MAX_MEMBERS];  // Member disk info
  BOOLEAN          Assembled;                 // TRUE if already assembled
} SDP_PENDING_ARRAY;

//
// Global driver state for event-based BlockIO notification
//
typedef struct {
  LIST_ENTRY  PendingArrayList;     // List of pending arrays by UUID
  EFI_EVENT   BlockIoNotifyEvent;   // Event for BlockIO notifications
  VOID        *BlockIoRegistration; // Registration handle
  EFI_EVENT   WorkerEvent;          // Worker event for assembly (avoid reentry)
  EFI_EVENT   EndOfDxeEvent;        // Optional: for diagnostics
} SDP_DRIVER_DATA;

//
// Array signature: "SDPA" (SDP Array)
//
#define SDP_ARRAY_SIGNATURE  SIGNATURE_32('S','D','P','A')

//
// CR macro to convert BlockIO protocol pointer to array private data
//
#define SDP_ARRAY_FROM_BLOCKIO(a) \
  CR(a, SDP_ARRAY_PRIVATE_DATA, BlockIo, SDP_ARRAY_SIGNATURE)

//
// RAID array private data (one instance per RAID array)
// This structure represents a virtual BlockIO device
//
typedef struct {
  UINT32    Signature;       // SDP_ARRAY_SIGNATURE

  //
  // Device handle and path for this virtual RAID device
  //
  EFI_HANDLE                Handle;       // Handle for virtual BlockIO device
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;  // Device path for virtual device

  //
  // Array identification
  //
  EFI_GUID  ArrayUuid;       // UUID of this array (from superblock)

  //
  // RAID configuration
  //
  UINT8   RaidLevel;       // RAID level (0/1/5)
  UINT8   NumMembers;      // Total number of member disks
  UINT8   PresentCount;    // Number of currently present/healthy disks
  UINT32  StripeSize;      // Stripe size in blocks

  //
  // Member disk information
  //
  SDP_MEMBER_DISK  *Members;      // Array of member disk info

  //
  // BlockIO protocol and media (exposed to OS)
  //
  EFI_BLOCK_IO_PROTOCOL  BlockIo;     // Virtual BlockIO protocol
  EFI_BLOCK_IO_MEDIA     Media;       // Media information

  //
  // DiskIO protocol (REQUIRED for PartitionDxe to bind!)
  //
  EFI_DISK_IO_PROTOCOL   DiskIo;      // Virtual DiskIO protocol

  //
  // Link to controller's array list
  //
  LIST_ENTRY  Link;         // Links all arrays managed by this controller
} SDP_ARRAY_PRIVATE_DATA;

//
// Controller signature: "SDPC" (SDP Controller)
//
#define SDP_CONTROLLER_SIGNATURE  SIGNATURE_32('S','D','P','C')

//
// Controller private data (one instance per PCI controller)
//
typedef struct {
  UINT32    Signature;       // SDP_CONTROLLER_SIGNATURE

  //
  // Controller handle and binding
  //
  EFI_HANDLE  ControllerHandle;      // Handle of PCI device
  EFI_HANDLE  DriverBindingHandle;   // Handle of driver binding

  //
  // PCI I/O protocol
  //
  EFI_PCI_IO_PROTOCOL  *PciIo;

  //
  // List of RAID arrays managed by this controller
  //
  LIST_ENTRY  ArrayListHead;

  //
  // Original PCI attributes (for restoration in Stop())
  //
  UINT64  OriginalPciAttributes;
} SDP_CONTROLLER_PRIVATE_DATA;

#endif // _SDP_RAID_H_
