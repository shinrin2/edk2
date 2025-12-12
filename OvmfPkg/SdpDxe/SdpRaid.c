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
  } else {
    DEBUG ((DEBUG_INFO, "SdpDxe: Driver binding installed successfully\n"));
  }

  return Status;
}
