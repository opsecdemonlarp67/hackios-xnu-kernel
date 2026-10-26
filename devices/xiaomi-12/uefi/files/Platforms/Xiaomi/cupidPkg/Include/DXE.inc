  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF Binaries/cupid/QcomPkg/Drivers/EnvDxe/EnvDxe.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  INF Binaries/cupid/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/cupid/QcomPkg/Drivers/TzDxe/ScmDxeLA.inf
  INF Binaries/cupid/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/cupid/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/ProjectInfoDxe/ProjectInfoDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf

  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/cupid/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/cupid/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/cupid/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/cupid/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF Binaries/cupid/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/cupid/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/cupid/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/RNGDxe/RngDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
  INF Binaries/cupid/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
