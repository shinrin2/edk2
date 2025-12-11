/** @file

  Skeleton PCIe RAID DXE driver.

  This driver currently only matches a configured Vendor/Device ID, opens
  EFI_PCI_IO_PROTOCOL, prints BAR information, and installs a private protocol
  to track that the device is being managed. Extend this file with actual RAID
  queue setup and BlockIo plumbing as you iterate.

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

#define PCIE_RAID_VENDOR_ID  0x1234
#define PCIE_RAID_DEVICE_ID  0x11AA

typedef struct {
  EFI_PCI_IO_PROTOCOL  *PciIo;
  EFI_PHYSICAL_ADDRESS Bar[PCI_MAX_BAR];
  UINT64               BarLength[PCI_MAX_BAR];
} PCIE_RAID_DEVICE;

EFI_GUID  gPcieRaidDeviceGuid = {
  0x8e8a02ad, 0x2bb3, 0x4e3f, { 0x9a, 0x6f, 0x3a, 0xd8, 0xf7, 0x5c, 0xa3, 0xd4 }
};

STATIC
EFI_STATUS
EFIAPI
PcieRaidSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE00           Pci;

  DEBUG ((DEBUG_INFO, "PcieRaid: Supported() called - checking device...\n"));

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_VERBOSE, "PcieRaid: Failed to open PciIo protocol: %r\n", Status));
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
    DEBUG ((DEBUG_INFO, "PcieRaid: Device VID=0x%04X DID=0x%04X\n", Pci.Hdr.VendorId, Pci.Hdr.DeviceId));
    if ((Pci.Hdr.VendorId == PCIE_RAID_VENDOR_ID) &&
        (Pci.Hdr.DeviceId == PCIE_RAID_DEVICE_ID))
    {
      DEBUG ((DEBUG_INFO, "PcieRaid: Device matched! VID=0x%04X DID=0x%04X\n", PCIE_RAID_VENDOR_ID, PCIE_RAID_DEVICE_ID));
      Status = EFI_SUCCESS;
    } else {
      DEBUG ((DEBUG_VERBOSE, "PcieRaid: Device not supported (expected VID=0x%04X DID=0x%04X)\n", PCIE_RAID_VENDOR_ID, PCIE_RAID_DEVICE_ID));
      Status = EFI_UNSUPPORTED;
    }
  } else {
    DEBUG ((DEBUG_ERROR, "PcieRaid: Failed to read PCI config space: %r\n", Status));
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
PcieRaidStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS          Status;
  EFI_PCI_IO_PROTOCOL *PciIo;
  PCIE_RAID_DEVICE    *RaidDevice;
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
    DEBUG ((DEBUG_ERROR, "PcieRaid: failed to enable device attributes: %r\n", Status));
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
      DEBUG ((DEBUG_INFO, "PcieRaid: BAR%u @ 0x%LX size 0x%Lx\n", (UINT32)Index, BarDesc->AddrRangeMin, BarDesc->AddrLen));
    }

    FreePool (BarDesc);
  }

  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gPcieRaidDeviceGuid,
                  EFI_NATIVE_INTERFACE,
                  RaidDevice
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PcieRaid: failed to install private protocol: %r\n", Status));
    goto ClosePciIo;
  }

  DEBUG ((DEBUG_INFO, "PcieRaid: driver started\n"));
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
PcieRaidStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS        Status;
  PCIE_RAID_DEVICE  *RaidDevice;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gPcieRaidDeviceGuid,
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
                  &gPcieRaidDeviceGuid,
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

STATIC EFI_DRIVER_BINDING_PROTOCOL  gPcieRaidDriverBinding = {
  PcieRaidSupported,
  PcieRaidStart,
  PcieRaidStop,
  0x10,
  NULL,
  NULL
};

STATIC EFI_UNICODE_STRING_TABLE  mPcieRaidDriverNameTable[] = {
  { "eng;en", L"PCIe RAID Driver (stub)" },
  { NULL,     NULL                      }
};

STATIC EFI_COMPONENT_NAME_PROTOCOL  gPcieRaidComponentName;

STATIC
EFI_STATUS
EFIAPI
PcieRaidGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mPcieRaidDriverNameTable,
           DriverName,
           (BOOLEAN)(This == &gPcieRaidComponentName)
           );
}

STATIC
EFI_STATUS
EFIAPI
PcieRaidGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_COMPONENT_NAME_PROTOCOL  gPcieRaidComponentName = {
  PcieRaidGetDriverName,
  PcieRaidGetControllerName,
  "eng"
};

STATIC EFI_COMPONENT_NAME2_PROTOCOL  gPcieRaidComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)PcieRaidGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME)PcieRaidGetControllerName,
  "en"
};

EFI_STATUS
EFIAPI
PcieRaidDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "PcieRaidDxe: EntryPoint called - driver loading...\n"));

  Status = EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &gPcieRaidDriverBinding,
           ImageHandle,
           &gPcieRaidComponentName,
           &gPcieRaidComponentName2
           );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PcieRaidDxe: Failed to install driver binding: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "PcieRaidDxe: Driver binding installed successfully\n"));
  }

  return Status;
}
