/** @file

  SDP RAID DXE driver.

  This driver implements software RAID0/RAID1/RAID5 for NVMe disks.
  It binds to the SDP RAID PCI card, enumerates physical BlockIo devices,
  reads superblock metadata from each disk, assembles RAID arrays, and
  creates virtual BlockIo devices for each array.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Pci.h>

#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/PciIo.h>

// 临时测试配置：绑定到 QEMU VGA 卡
// TODO Phase 10: 改回 0x1234/0x11AA
#define SDP_RAID_VENDOR_ID  0x1234
#define SDP_RAID_DEVICE_ID  0x1111  // 临时改为 VGA 卡的 DID

typedef struct {
  EFI_PCI_IO_PROTOCOL  *PciIo;
  EFI_PHYSICAL_ADDRESS Bar[PCI_MAX_BAR];
  UINT64               BarLength[PCI_MAX_BAR];
} SDP_RAID_DEVICE;

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
  EFI_STATUS          Status;
  EFI_PCI_IO_PROTOCOL *PciIo;
  SDP_RAID_DEVICE    *RaidDevice;
  UINT64              Attributes;
  UINTN               Index;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *BarDesc;

  RaidDevice = AllocateZeroPool (sizeof (*RaidDevice));
  if (RaidDevice == NULL) {
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

  RaidDevice->PciIo = PciIo;

  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationGet,
                    0,
                    &Attributes
                    );
  if (!EFI_ERROR (Status)) {
    Status = PciIo->Attributes (
                      PciIo,
                      EfiPciIoAttributeOperationEnable,
                      Attributes | EFI_PCI_DEVICE_ENABLE,
                      NULL
                      );
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: failed to enable device attributes: %r\n", Status));
    goto ClosePciIo;
  }

  for (Index = 0; Index < PCI_MAX_BAR; Index++) {
    Status = PciIo->GetBarAttributes (PciIo, Index, NULL, (VOID **)&BarDesc);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (BarDesc->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
      RaidDevice->Bar[Index]      = BarDesc->AddrRangeMin;
      RaidDevice->BarLength[Index] = BarDesc->AddrLen;
      DEBUG ((DEBUG_INFO, "SdpDxe: BAR%u @ 0x%LX size 0x%Lx\n", (UINT32)Index, BarDesc->AddrRangeMin, BarDesc->AddrLen));
    }

    FreePool (BarDesc);
  }

  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gSdpRaidDeviceGuid,
                  EFI_NATIVE_INTERFACE,
                  RaidDevice
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdpDxe: failed to install private protocol: %r\n", Status));
    goto ClosePciIo;
  }

  DEBUG ((DEBUG_INFO, "SdpDxe: driver started\n"));
  return EFI_SUCCESS;

ClosePciIo:
  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

FreeDevice:
  FreePool (RaidDevice);
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
  EFI_STATUS        Status;
  SDP_RAID_DEVICE  *RaidDevice;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gSdpRaidDeviceGuid,
                  (VOID **)&RaidDevice,
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
                  RaidDevice
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

  FreePool (RaidDevice);
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
