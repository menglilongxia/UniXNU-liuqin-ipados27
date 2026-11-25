##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = liuqin
  PLATFORM_GUID                  = 9C4B7E21-5F38-4A9D-8E12-6B0A4C73E901
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/liuqinPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = liuqinPkg/liuqin.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = Pure Mu-Silicium open-source drivers only (SimpleFbDxe on ABL's live
  #     0xB8000000 continuous splash framebuffer). Avoids vendor DXE version
  #     mismatches across HyperOS 1/2/3 firmware builds.
  # 1 = Load stock vendor UEFI DXE modules imported from matching uefi.elf.
  #
  LIUQIN_VENDOR_HW               = 0

  #
  # Qualcomm Waipio/Cape (SM8450/SM8475) silicon package.
  #
  SOC_TYPE                       = 0

!include WaipioPkg/WaipioPkg.dsc.inc

!if ($(USE_CUSTOM_DISPLAY_DRIVER) == 1) && ($(LIUQIN_VENDOR_HW) == 0)
  !error The stock display chain needs LIUQIN_VENDOR_HW = 1
!endif

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack (matches stock liuqin uefiplat.cfg)
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Xiaomi Pad 6 Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"23046RP50C"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"liuqin"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Xiaomi Pad 6 Pro"

  #
  # Simple Frame Buffer (ABL continuous splash: 0xB8000000, 1800x2880, 32-bit)
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1800
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2880
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferFontScale|3
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|0
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"
!if $(LIUQIN_VENDOR_HW) == 0
  gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|FALSE
!endif

[LibraryClasses]
  MemoryMapLib|liuqinPkg/Library/MemoryMapLib/MemoryMapLib.inf
  ConfigurationMapLib|liuqinPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

[Components]
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
