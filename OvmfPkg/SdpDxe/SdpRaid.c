/** @file

  SDP RAID DXE driver.

  This driver implements software RAID0/RAID1/RAID5 for NVMe disks.
  It binds to the SDP RAID PCI card, enumerates physical BlockIo devices,
  reads superblock metadata from each disk, assembles RAID arrays, and
  creates virtual BlockIo devices for each array.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "SdpRaid.h"

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Pci.h>

// 临时测试配置：绑定到 QEMU VGA 卡
// TODO Phase 10: 改回 0x1234/0x11AA
#define SDP_RAID_VENDOR_ID  0x1234
#define SDP_RAID_DEVICE_ID  0x1111  // 临时改为 VGA 卡的 DID

// Note: gSdpRaidDeviceGuid is defined in OvmfPkg.dec and auto-generated in AutoGen.c

//
// Global driver data for event-based BlockIO notification (Phase 4)
//
STATIC SDP_DRIVER_DATA  mDriverData;

//
// Forward declarations for event callbacks
//
STATIC VOID EFIAPI SdpBlockIoNotifyCallback (IN EFI_EVENT Event, IN VOID *Context);
STATIC VOID EFIAPI SdpWorkerCallback (IN EFI_EVENT Event, IN VOID *Context);
STATIC VOID EFIAPI SdpEndOfDxeCallback (IN EFI_EVENT Event, IN VOID *Context);

/**
  Validate a superblock structure.

  @param[in]  Superblock  Pointer to superblock to validate.

  @retval TRUE   Superblock is valid.
  @retval FALSE  Superblock is invalid.
**/
STATIC
BOOLEAN
SdpValidateSuperblock (
  IN CONST SDP_SUPERBLOCK  *Superblock
  )
{
  UINT32  CalculatedChecksum;
  UINT32  StoredChecksum;
  UINTN   Index;
  UINT8   *Bytes;

  //
  // Check magic number
  //
  if (Superblock->Magic != SDP_SUPERBLOCK_MAGIC) {
    DEBUG ((DEBUG_VERBOSE, "SdpDxe: Invalid magic: 0x%08X (expected 0x%08X)\n", Superblock->Magic, SDP_SUPERBLOCK_MAGIC));
    return FALSE;
  }

  //
  // Check version
  //
  if (Superblock->Version != SDP_SUPERBLOCK_VERSION) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Unsupported version: %u (expected %u)\n", Superblock->Version, SDP_SUPERBLOCK_VERSION));
    return FALSE;
  }

  //
  // Validate RAID level
  //
  if ((Superblock->RaidLevel != SDP_RAID_LEVEL_0) &&
      (Superblock->RaidLevel != SDP_RAID_LEVEL_1) &&
      (Superblock->RaidLevel != SDP_RAID_LEVEL_5))
  {
    DEBUG ((DEBUG_WARN, "SdpDxe: Invalid RAID level: %u\n", Superblock->RaidLevel));
    return FALSE;
  }

  //
  // Validate member count
  //
  if ((Superblock->NumMembers < 2) || (Superblock->NumMembers > 16)) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Invalid member count: %u\n", Superblock->NumMembers));
    return FALSE;
  }

  //
  // Validate member index
  //
  if (Superblock->MemberIndex >= Superblock->NumMembers) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Invalid member index: %u (NumMembers=%u)\n", Superblock->MemberIndex, Superblock->NumMembers));
    return FALSE;
  }

  //
  // Verify checksum
  //
  StoredChecksum     = Superblock->Checksum;
  CalculatedChecksum = 0;
  Bytes              = (UINT8 *)Superblock;

  for (Index = 0; Index < sizeof (SDP_SUPERBLOCK); Index++) {
    //
    // Skip checksum field itself (bytes 48-51 in the structure)
    // Offset calculation: Magic(4) + Version(4) + ArrayUuid(16) +
    //   RaidLevel(1) + NumMembers(1) + MemberIndex(1) + Reserved1(1) +
    //   ArraySizeInBlocks(8) + MemberOffsetInBlocks(8) + StripeSize(4) = 48
    //
    if ((Index >= 48) && (Index < 52)) {
      continue;
    }

    CalculatedChecksum = (CalculatedChecksum + Bytes[Index]) & 0xFFFFFFFF;
  }

  if (CalculatedChecksum != StoredChecksum) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Checksum mismatch: 0x%08X (expected 0x%08X)\n", CalculatedChecksum, StoredChecksum));
    return FALSE;
  }

  return TRUE;
}

/**
  Read and validate superblock from a BlockIO device.

  @param[in]   BlockIo      BlockIO protocol pointer.
  @param[out]  Superblock   Buffer to receive superblock (must be 512 bytes).

  @retval EFI_SUCCESS       Superblock read and validated successfully.
  @retval EFI_NOT_FOUND     No valid superblock found.
  @retval Others            Error reading from device.
**/
STATIC
EFI_STATUS
SdpReadSuperblock (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  OUT SDP_SUPERBLOCK         *Superblock
  )
{
  EFI_STATUS  Status;
  UINT64      SuperblockLba;
  VOID        *Buffer;
  BOOLEAN     UsedAlignedPages;

  //
  // Superblock is at second-to-last block (LastBlock - 1)
  //
  if (BlockIo->Media->LastBlock < 2) {
    DEBUG ((DEBUG_VERBOSE, "SdpDxe: Device too small for superblock\n"));
    return EFI_NOT_FOUND;
  }

  SuperblockLba = BlockIo->Media->LastBlock - 1;

  //
  // Allocate aligned buffer for reading
  // Note: Some BlockIo implementations require IoAlign
  // IMPORTANT: Track allocation type so we free correctly!
  //
  UsedAlignedPages = (BlockIo->Media->IoAlign > 1);
  if (UsedAlignedPages) {
    Buffer = AllocateAlignedPages (EFI_SIZE_TO_PAGES (512), BlockIo->Media->IoAlign);
  } else {
    Buffer = AllocatePool (512);
  }

  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Read superblock
  //
  Status = BlockIo->ReadBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      SuperblockLba,
                      512,
                      Buffer
                      );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Failed to read superblock at LBA 0x%lx: %r\n", SuperblockLba, Status));
    if (UsedAlignedPages) {
      FreePages (Buffer, EFI_SIZE_TO_PAGES (512));
    } else {
      FreePool (Buffer);
    }
    return Status;
  }

  //
  // Copy to output buffer
  //
  CopyMem (Superblock, Buffer, sizeof (SDP_SUPERBLOCK));
  if (UsedAlignedPages) {
    FreePages (Buffer, EFI_SIZE_TO_PAGES (512));
  } else {
    FreePool (Buffer);
  }

  //
  // Validate superblock
  //
  if (!SdpValidateSuperblock (Superblock)) {
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: Valid superblock found at LBA 0x%lx\n", SuperblockLba));
  return EFI_SUCCESS;
}

//
// ============================================================================
// Phase 4: Event-based BlockIO notification helpers and callbacks
// ============================================================================
//

/**
  Check if a handle represents an NVMe namespace device.
  This filters out USB/SATA/virtio and other non-NVMe BlockIO devices.

  @param[in]  Handle  Handle to check.

  @retval TRUE   Handle is an NVMe namespace.
  @retval FALSE  Handle is not an NVMe namespace.
**/
STATIC
BOOLEAN
SdpIsNvmeNamespace (
  IN EFI_HANDLE  Handle
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  //
  // Walk device path looking for NVMe Namespace node
  // MSG_NVME_NAMESPACE_DP = 0x17 (defined in DevicePath.h)
  //
  for (Node = DevicePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MSG_NVME_NAMESPACE_DP))
    {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Find an existing pending array by UUID, or create a new one.

  @param[in]  ArrayUuid   UUID to search for.
  @param[in]  Superblock  Superblock from the first member (used to initialize new array).

  @return  Pointer to pending array structure.
**/
STATIC
SDP_PENDING_ARRAY *
SdpFindOrCreatePendingArray (
  IN CONST EFI_GUID        *ArrayUuid,
  IN CONST SDP_SUPERBLOCK  *Superblock
  )
{
  LIST_ENTRY         *Entry;
  SDP_PENDING_ARRAY  *PendingArray;

  //
  // Search existing pending arrays by UUID
  //
  for (Entry = GetFirstNode (&mDriverData.PendingArrayList);
       !IsNull (&mDriverData.PendingArrayList, Entry);
       Entry = GetNextNode (&mDriverData.PendingArrayList, Entry))
  {
    PendingArray = BASE_CR (Entry, SDP_PENDING_ARRAY, Link);
    if (CompareGuid (&PendingArray->ArrayUuid, ArrayUuid)) {
      return PendingArray;
    }
  }

  //
  // Not found, create new one
  //
  PendingArray = AllocateZeroPool (sizeof (*PendingArray));
  if (PendingArray == NULL) {
    return NULL;
  }

  CopyGuid (&PendingArray->ArrayUuid, ArrayUuid);
  PendingArray->RaidLevel  = Superblock->RaidLevel;
  PendingArray->NumMembers = Superblock->NumMembers;
  PendingArray->StripeSize = Superblock->StripeSize;
  PendingArray->FoundCount = 0;
  PendingArray->Assembled  = FALSE;

  InsertTailList (&mDriverData.PendingArrayList, &PendingArray->Link);

  DEBUG ((
    DEBUG_INFO,
    "SdpDxe: Created pending array for UUID, RAID%u with %u members\n",
    PendingArray->RaidLevel,
    PendingArray->NumMembers
    ));

  return PendingArray;
}

/**
  Worker callback - assembles RAID arrays when all members are found.
  This is called via SignalEvent from the notify callback to avoid reentry issues.

  @param[in]  Event    Event that was signaled.
  @param[in]  Context  Not used.
**/
STATIC
VOID
EFIAPI
SdpWorkerCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  LIST_ENTRY         *Entry;
  SDP_PENDING_ARRAY  *PendingArray;

  DEBUG ((DEBUG_INFO, "SdpDxe: Worker callback - checking for complete arrays\n"));

  for (Entry = GetFirstNode (&mDriverData.PendingArrayList);
       !IsNull (&mDriverData.PendingArrayList, Entry);
       Entry = GetNextNode (&mDriverData.PendingArrayList, Entry))
  {
    PendingArray = BASE_CR (Entry, SDP_PENDING_ARRAY, Link);

    if ((PendingArray->FoundCount == PendingArray->NumMembers) &&
        !PendingArray->Assembled)
    {
      DEBUG ((
        DEBUG_INFO,
        "SdpDxe: Array complete! RAID%u with %u members - ready to assemble\n",
        PendingArray->RaidLevel,
        PendingArray->NumMembers
        ));

      //
      // Mark as assembled to prevent re-trigger
      // TODO: Implement actual array assembly (SdpAssembleArray)
      //
      PendingArray->Assembled = TRUE;

      //
      // For now, just log success - actual assembly will be implemented later
      //
      DEBUG ((DEBUG_INFO, "SdpDxe: [PLACEHOLDER] Would assemble RAID array here\n"));
    }
  }
}

/**
  EndOfDxe callback for diagnostics.
  Reports any incomplete arrays (missing members).

  @param[in]  Event    Event that was signaled.
  @param[in]  Context  Not used.
**/
STATIC
VOID
EFIAPI
SdpEndOfDxeCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  LIST_ENTRY         *Entry;
  SDP_PENDING_ARRAY  *PendingArray;
  UINTN              IncompleteCount;

  IncompleteCount = 0;

  DEBUG ((DEBUG_INFO, "SdpDxe: EndOfDxe - checking array status\n"));

  for (Entry = GetFirstNode (&mDriverData.PendingArrayList);
       !IsNull (&mDriverData.PendingArrayList, Entry);
       Entry = GetNextNode (&mDriverData.PendingArrayList, Entry))
  {
    PendingArray = BASE_CR (Entry, SDP_PENDING_ARRAY, Link);

    if (!PendingArray->Assembled) {
      DEBUG ((
        DEBUG_WARN,
        "SdpDxe: Array incomplete! Found %u/%u members (RAID%u)\n",
        PendingArray->FoundCount,
        PendingArray->NumMembers,
        PendingArray->RaidLevel
        ));
      IncompleteCount++;
    }
  }

  if (IncompleteCount == 0) {
    DEBUG ((DEBUG_INFO, "SdpDxe: All discovered arrays are complete\n"));
  }
}

/**
  BlockIO notification callback.
  Called when new BlockIO protocols are installed. Discovers RAID member disks.
  Only collects members - actual assembly is done in worker callback.

  @param[in]  Event    Event that was signaled.
  @param[in]  Context  Not used.
**/
STATIC
VOID
EFIAPI
SdpBlockIoNotifyCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             Handle;
  UINTN                  BufferSize;
  VOID                   *Dummy;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  SDP_SUPERBLOCK         Superblock;
  SDP_PENDING_ARRAY      *PendingArray;
  BOOLEAN                NeedWorker;

  NeedWorker = FALSE;

  DEBUG ((DEBUG_INFO, "SdpDxe: BlockIO notify callback triggered\n"));

  //
  // Loop to process ALL newly-arrived handles (callback may batch multiple)
  //
  while (TRUE) {
    BufferSize = sizeof (Handle);

    //
    // Locate newly-arrived BlockIO handles
    // CRITICAL: Second param MUST be protocol GUID, not NULL!
    //
    Status = gBS->LocateHandle (
                    ByRegisterNotify,
                    &gEfiBlockIoProtocolGuid,
                    mDriverData.BlockIoRegistration,
                    &BufferSize,
                    &Handle
                    );

    //
    // EFI_NOT_FOUND is normal - means no more new handles in this batch
    // Other errors should be logged for debugging
    //
    if (Status == EFI_NOT_FOUND) {
      break;  // Normal: no more new handles
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SdpDxe: LocateHandle error: %r\n", Status));
      break;
    }

    DEBUG ((DEBUG_INFO, "SdpDxe: Processing new BlockIO handle 0x%p\n", Handle));

    //
    // Filter: Skip our own virtual disks (use HandleProtocol, simpler)
    //
    Status = gBS->HandleProtocol (Handle, &gSdpRaidVirtualDiskGuid, &Dummy);
    if (!EFI_ERROR (Status)) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: Skipping our own virtual disk\n"));
      continue;
    }

    //
    // Filter: Only process NVMe namespace devices
    //
    if (!SdpIsNvmeNamespace (Handle)) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: Skipping non-NVMe device\n"));
      continue;
    }

    DEBUG ((DEBUG_INFO, "SdpDxe: Found NVMe namespace device\n"));

    //
    // Get BlockIO and check media properties
    //
    Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (BlockIo->Media->LogicalPartition) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: Skipping logical partition\n"));
      continue;
    }

    if (BlockIo->Media->ReadOnly) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: Skipping read-only device\n"));
      continue;
    }

    //
    // Read and validate superblock
    //
    Status = SdpReadSuperblock (BlockIo, &Superblock);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: No valid superblock (not a RAID member)\n"));
      continue;
    }

    DEBUG ((DEBUG_INFO, "SdpDxe: Found RAID member disk!\n"));

    //
    // Bounds check MemberIndex AND NumMembers
    //
    if ((Superblock.MemberIndex >= SDP_MAX_MEMBERS) ||
        (Superblock.NumMembers > SDP_MAX_MEMBERS))
    {
      DEBUG ((
        DEBUG_ERROR,
        "SdpDxe: Invalid MemberIndex %u or NumMembers %u, reject\n",
        Superblock.MemberIndex,
        Superblock.NumMembers
        ));
      continue;
    }

    //
    // Find or create pending array by UUID
    //
    PendingArray = SdpFindOrCreatePendingArray (&Superblock.ArrayUuid, &Superblock);
    if (PendingArray == NULL) {
      DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to allocate pending array\n"));
      continue;
    }

    //
    // Consistency check: Verify parameters match existing array
    //
    if ((PendingArray->RaidLevel != Superblock.RaidLevel) ||
        (PendingArray->NumMembers != Superblock.NumMembers) ||
        (PendingArray->StripeSize != Superblock.StripeSize))
    {
      DEBUG ((
        DEBUG_ERROR,
        "SdpDxe: Member params mismatch! Reject member %u\n",
        Superblock.MemberIndex
        ));
      continue;
    }

    //
    // Deduplicate: Skip if this member slot already filled
    //
    if (PendingArray->Members[Superblock.MemberIndex].Present) {
      DEBUG ((
        DEBUG_WARN,
        "SdpDxe: Duplicate member %u for array, skip\n",
        Superblock.MemberIndex
        ));
      continue;
    }

    //
    // Add member to pending array (store Handle, not protocol pointer!)
    //
    PendingArray->Members[Superblock.MemberIndex].Handle      = Handle;
    PendingArray->Members[Superblock.MemberIndex].MemberIndex = Superblock.MemberIndex;
    PendingArray->Members[Superblock.MemberIndex].Present     = TRUE;
    CopyMem (
      &PendingArray->Members[Superblock.MemberIndex].Superblock,
      &Superblock,
      sizeof (Superblock)
      );
    PendingArray->FoundCount++;

    DEBUG ((
      DEBUG_INFO,
      "SdpDxe: Added member %u/%u for array (RAID%u)\n",
      PendingArray->FoundCount,
      PendingArray->NumMembers,
      PendingArray->RaidLevel
      ));

    //
    // Check if ready to assemble (don't assemble here - signal worker)
    //
    if ((PendingArray->FoundCount == PendingArray->NumMembers) &&
        !PendingArray->Assembled)
    {
      NeedWorker = TRUE;
    }
  }

  //
  // Signal worker event to do assembly (outside the loop, avoids reentry)
  //
  if (NeedWorker) {
    DEBUG ((DEBUG_INFO, "SdpDxe: Signaling worker to assemble array\n"));
    gBS->SignalEvent (mDriverData.WorkerEvent);
  }
}

/**
  Enumerate all BlockIO devices in the system and identify potential RAID members.

  @param[in]  Controller  Controller private data.

  @retval EFI_SUCCESS     Successfully enumerated BlockIO devices.
  @retval Others          Failed to enumerate devices.
**/
STATIC
EFI_STATUS
SdpEnumeratePhysicalDisks (
  IN SDP_CONTROLLER_PRIVATE_DATA  *Controller
  )
{
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  EFI_HANDLE                *HandleBuffer;
  UINTN                     Index;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  CHAR16                    *DevicePathText;
  UINTN                     CandidateCount;

  HandleCount    = 0;
  HandleBuffer   = NULL;
  CandidateCount = 0;

  DEBUG ((DEBUG_INFO, "SdpDxe: Enumerating BlockIo devices...\n"));

  //
  // Locate all handles that support BlockIO protocol
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    if (Status == EFI_NOT_FOUND) {
      DEBUG ((DEBUG_INFO, "SdpDxe: No BlockIo devices found yet (drivers may still be loading)\n"));
      return EFI_SUCCESS;  // Not an error - BlockIo drivers may load later
    }

    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to locate BlockIo handles: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: Found %u BlockIo devices\n", HandleCount));

  //
  // Iterate through all BlockIO devices
  //
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Filter: Skip logical partitions and read-only devices
    //
    if (BlockIo->Media->LogicalPartition) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: [%u] Skipping logical partition\n", Index));
      continue;
    }

    if (BlockIo->Media->ReadOnly) {
      DEBUG ((DEBUG_VERBOSE, "SdpDxe: [%u] Skipping read-only device\n", Index));
      continue;
    }

    //
    // Get device path for printing
    //
    DevicePathText = NULL;
    Status         = gBS->HandleProtocol (
                            HandleBuffer[Index],
                            &gEfiDevicePathProtocolGuid,
                            (VOID **)&DevicePath
                            );
    if (!EFI_ERROR (Status)) {
      DevicePathText = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
    }

    //
    // Print device information
    //
    DEBUG ((DEBUG_INFO, "SdpDxe: [%u] Handle=0x%p\n", Index, HandleBuffer[Index]));
    DEBUG ((
      DEBUG_INFO,
      "       Media: BlockSize=%u, LastBlock=0x%lx\n",
      BlockIo->Media->BlockSize,
      BlockIo->Media->LastBlock
      ));
    DEBUG ((
      DEBUG_INFO,
      "       LogicalPartition=%u, ReadOnly=%u\n",
      BlockIo->Media->LogicalPartition,
      BlockIo->Media->ReadOnly
      ));

    if (DevicePathText != NULL) {
      DEBUG ((DEBUG_INFO, "       DevPath: %s\n", DevicePathText));
      FreePool (DevicePathText);
    }

    DEBUG ((DEBUG_INFO, "       -> Candidate for RAID member\n"));

    //
    // Phase 3: Try to read superblock from this device
    //
    SDP_SUPERBLOCK  Superblock;
    Status = SdpReadSuperblock (BlockIo, &Superblock);

    if (!EFI_ERROR (Status)) {
      CHAR8  UuidStr[37];

      //
      // Format UUID for display
      //
      AsciiSPrint (
        UuidStr,
        sizeof (UuidStr),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        Superblock.ArrayUuid.Data1,
        Superblock.ArrayUuid.Data2,
        Superblock.ArrayUuid.Data3,
        Superblock.ArrayUuid.Data4[0],
        Superblock.ArrayUuid.Data4[1],
        Superblock.ArrayUuid.Data4[2],
        Superblock.ArrayUuid.Data4[3],
        Superblock.ArrayUuid.Data4[4],
        Superblock.ArrayUuid.Data4[5],
        Superblock.ArrayUuid.Data4[6],
        Superblock.ArrayUuid.Data4[7]
        );

      DEBUG ((DEBUG_INFO, "       *** FOUND RAID MEMBER ***\n"));
      DEBUG ((DEBUG_INFO, "       ArrayUUID: %a\n", UuidStr));
      DEBUG ((DEBUG_INFO, "       RAID Level: %u\n", Superblock.RaidLevel));
      DEBUG ((DEBUG_INFO, "       Member: %u/%u (index %u of %u total)\n", Superblock.MemberIndex + 1, Superblock.NumMembers, Superblock.MemberIndex, Superblock.NumMembers));
      DEBUG ((DEBUG_INFO, "       Array Size: %lu blocks (%lu MB)\n", Superblock.ArraySizeInBlocks, (Superblock.ArraySizeInBlocks * 512) / (1024 * 1024)));
      DEBUG ((DEBUG_INFO, "       Stripe Size: %u blocks\n", Superblock.StripeSize));

      CandidateCount++;
    } else {
      DEBUG ((DEBUG_VERBOSE, "       No valid superblock (not a RAID member)\n"));
    }
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: Found %u RAID member disks\n", CandidateCount));

  if (HandleBuffer != NULL) {
    FreePool (HandleBuffer);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SdpRaidSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE00           Pci;

  DEBUG ((DEBUG_INFO, "SdpDxe: Supported() called - checking device...\n"));

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_VERBOSE, "SdpDxe: Failed to open PciIo protocol: %r\n", Status));
    return Status;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        0,
                        sizeof (Pci) / sizeof (UINT32),
                        &Pci
                        );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SdpDxe: Device VID=0x%04X DID=0x%04X\n", Pci.Hdr.VendorId, Pci.Hdr.DeviceId));
    if ((Pci.Hdr.VendorId == SDP_RAID_VENDOR_ID) &&
        (Pci.Hdr.DeviceId == SDP_RAID_DEVICE_ID))
    {
      DEBUG ((DEBUG_INFO, "SdpDxe: Device matched! VID=0x%04X DID=0x%04X\n", SDP_RAID_VENDOR_ID, SDP_RAID_DEVICE_ID));
      Status = EFI_SUCCESS;
    } else {
      DEBUG ((DEBUG_INFO, "SdpDxe: Device not supported (expected VID=0x%04X DID=0x%04X)\n", SDP_RAID_VENDOR_ID, SDP_RAID_DEVICE_ID));
      Status = EFI_UNSUPPORTED;
    }
  } else {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to read PCI config space: %r\n", Status));
  }

  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SdpRaidStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                      Status;
  EFI_PCI_IO_PROTOCOL             *PciIo;
  SDP_CONTROLLER_PRIVATE_DATA     *Private;
  UINT64                          Attributes;

  Private = AllocateZeroPool (sizeof (*Private));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto FreeDevice;
  }

  //
  // 临时测试模式警告：绑定到 VGA 卡而非真实 RAID 卡
  //
  DEBUG ((DEBUG_WARN, "SdpDxe: [TEST MODE] Bound to VID=0x%04X DID=0x%04X (not real RAID card)\n",
          SDP_RAID_VENDOR_ID, SDP_RAID_DEVICE_ID));

  //
  // Initialize controller private data
  //
  Private->Signature            = SDP_CONTROLLER_SIGNATURE;
  Private->ControllerHandle     = Controller;
  Private->DriverBindingHandle  = This->DriverBindingHandle;
  Private->PciIo                = PciIo;
  InitializeListHead (&Private->ArrayListHead);

  //
  // Phase 1 verification: Print structure sizes
  //
  DEBUG ((DEBUG_INFO, "SdpDxe: Phase 1 - Data structure sizes:\n"));
  DEBUG ((DEBUG_INFO, "  SDP_SUPERBLOCK: %u bytes\n", sizeof (SDP_SUPERBLOCK)));
  DEBUG ((DEBUG_INFO, "  SDP_MEMBER_DISK: %u bytes\n", sizeof (SDP_MEMBER_DISK)));
  DEBUG ((DEBUG_INFO, "  SDP_ARRAY_PRIVATE_DATA: %u bytes\n", sizeof (SDP_ARRAY_PRIVATE_DATA)));
  DEBUG ((DEBUG_INFO, "  SDP_CONTROLLER_PRIVATE_DATA: %u bytes\n", sizeof (SDP_CONTROLLER_PRIVATE_DATA)));

  //
  // Phase 2: Enumerate all BlockIo physical disks
  // NOTE: This is called early in Start(), so NVMe drivers may not have
  //       installed BlockIo protocols yet. Phase 3 will implement event
  //       notification to enumerate devices at the right time.
  //
  Status = SdpEnumeratePhysicalDisks (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Failed to enumerate physical disks: %r\n", Status));
    // Continue anyway - this is not a fatal error for Phase 2
  }

  //
  // Save original PCI attributes for restoration in Stop()
  //
  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationGet,
                    0,
                    &Attributes
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to get PCI attributes: %r\n", Status));
    goto ClosePciIo;
  }

  Private->OriginalPciAttributes = Attributes;

  //
  // Enable PCI device
  //
  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationEnable,
                    Attributes | EFI_PCI_DEVICE_ENABLE,
                    NULL
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to enable device attributes: %r\n", Status));
    goto ClosePciIo;
  }

  //
  // Install private protocol on controller handle
  //
  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gSdpRaidDeviceGuid,
                  EFI_NATIVE_INTERFACE,
                  Private
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to install private protocol: %r\n", Status));
    goto ClosePciIo;
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: Controller initialized successfully (Phase 1 complete)\n"));
  return EFI_SUCCESS;

ClosePciIo:
  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

FreeDevice:
  FreePool (Private);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SdpRaidStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                   Status;
  SDP_CONTROLLER_PRIVATE_DATA  *Private;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gSdpRaidDeviceGuid,
                  (VOID **)&Private,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->UninstallProtocolInterface (
                  Controller,
                  &gSdpRaidDeviceGuid,
                  Private
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CloseProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  This->DriverBindingHandle,
                  Controller
                  );

  FreePool (Private);
  return Status;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  gSdpRaidDriverBinding = {
  SdpRaidSupported,
  SdpRaidStart,
  SdpRaidStop,
  0x10,
  NULL,
  NULL
};

STATIC EFI_UNICODE_STRING_TABLE  mSdpRaidDriverNameTable[] = {
  { "eng;en", L"SDP RAID Driver" },
  { NULL,     NULL                }
};

STATIC EFI_COMPONENT_NAME_PROTOCOL  gSdpRaidComponentName;

STATIC
EFI_STATUS
EFIAPI
SdpRaidGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mSdpRaidDriverNameTable,
           DriverName,
           (BOOLEAN)(This == &gSdpRaidComponentName)
           );
}

STATIC
EFI_STATUS
EFIAPI
SdpRaidGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_COMPONENT_NAME_PROTOCOL  gSdpRaidComponentName = {
  SdpRaidGetDriverName,
  SdpRaidGetControllerName,
  "eng"
};

STATIC EFI_COMPONENT_NAME2_PROTOCOL  gSdpRaidComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)SdpRaidGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME)SdpRaidGetControllerName,
  "en"
};

EFI_STATUS
EFIAPI
SdpDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "SdpDxe: EntryPoint called - driver loading...\n"));

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gSdpRaidDriverBinding,
             ImageHandle,
             &gSdpRaidComponentName,
             &gSdpRaidComponentName2
             );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to install driver binding: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: Driver binding installed successfully\n"));

  //
  // Phase 4: Initialize event-based BlockIO notification
  //
  DEBUG ((DEBUG_INFO, "SdpDxe: Initializing event-based BlockIO discovery...\n"));

  //
  // Initialize global driver data
  //
  InitializeListHead (&mDriverData.PendingArrayList);

  //
  // Create worker event for array assembly (avoids reentry issues in notify callback)
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  SdpWorkerCallback,
                  NULL,
                  &mDriverData.WorkerEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to create worker event: %r\n", Status));
    return Status;
  }

  //
  // Register for BlockIO protocol notifications using EfiCreateProtocolNotifyEvent
  // This is the EDK2 helper that handles the boilerplate
  //
  mDriverData.BlockIoNotifyEvent = EfiCreateProtocolNotifyEvent (
                                     &gEfiBlockIoProtocolGuid,
                                     TPL_CALLBACK,
                                     SdpBlockIoNotifyCallback,
                                     NULL,
                                     &mDriverData.BlockIoRegistration
                                     );
  if (mDriverData.BlockIoNotifyEvent == NULL) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: Failed to create BlockIO notify event\n"));
    gBS->CloseEvent (mDriverData.WorkerEvent);
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: BlockIO notification registered\n"));

  //
  // CRITICAL: Signal event once to process already-existing BlockIO devices
  // (If NVMe driver loaded before us, we'd miss those devices otherwise)
  //
  gBS->SignalEvent (mDriverData.BlockIoNotifyEvent);

  //
  // Optional: Register EndOfDxe event for diagnostics
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  SdpEndOfDxeCallback,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &mDriverData.EndOfDxeEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SdpDxe: Failed to register EndOfDxe event: %r (non-fatal)\n", Status));
    // Not fatal - continue without diagnostics
  } else {
    DEBUG ((DEBUG_INFO, "SdpDxe: EndOfDxe diagnostic event registered\n"));
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: Phase 4 initialization complete\n"));

  return EFI_SUCCESS;
}
