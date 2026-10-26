# Device: Xiaomi 12 (cupid)

Everything in HakiOS that belongs to one phone is here. The kernel, loader and user space above
know nothing about the phone: addresses, interrupts, power rails and firmware come from its device
tree and its Android.

| What | Where |
|---|---|
| SoC | Qualcomm Snapdragon 8 Gen 1 (SM8450, "waipio" in the device tree) |
| UEFI for the phone | `uefi/`: a port of Mu-Silicium (a patch and files; `apply.sh` clones and applies, `export.sh` saves) |
| Prebuilt UEFI images | `prebuilt/`: `SILICIUM_UEFI.fd`, `BootShim.bin`, the device tree `device-tree.dtb` (from `vendor_boot`), `ramdisk`, `mkbootimg.py`, and the cupid loader/kernel (`loader.efi`, `mach.development.bcm2837`) |
| The phone's firmware | **not in the repository**: `pull-firmware.sh` pulls it off your phone over `adb` into `fw-*/` at the top |
| SoC boot-args | `soc.args`: the modem/IPA addresses from this phone's device tree (`tools/soc-args.py`, `pull-soc.sh`) |

The phone's own device tree (`vendor_boot`/`dtbo`) is `prebuilt/device-tree.dtb`; the decoded copy
used while porting is `~/hackios-cupid/devices/cupid/cupid.dts`.

## Status on this phone

Ported and building:

- **UEFI**: `cupidPkg` with cupid's memory map, built from the `ingres` (POCO F4 GT) driver set at
  the same SM8450/waipio. FD `0xA7000000`/`0x400000`, XNUP payload at `0xF4600000`, DXE heap at
  `0xF8600000`.
- **Loader**: `platform_cupid` (GIC `0x17100000`/`0x17180000`, 256 KB redistributor stride, 8 cores,
  framebuffer `0xB8000000`), selected by the framebuffer base.
- **Touch**: `hakios.fts.c`, the ST FTS controller on QUP0 SE4 (`spi@990000`), with the PM8350C
  LDO2/LDO7 supplies and TLMM pins 16-21.
- **Buttons**: `hakios.buttons.cupid.c`, a separate driver for the SPMI arbiter v7 (PMK8350 PON,
  PM8350 GPIO6), dispatched by `hakios.buttons.c` on the cupid SoC base.
- **CPU frequency**: `hakios.cpufreq.c` selects cupid's EPSS (`0x17d91000`) and RSC (`0x17a20000`).

Not ported yet (the redwood drivers keep themselves off cupid by the SoC base):

- GPU (Adreno 730), Wi-Fi (WCN6855/QCA6490), audio (ADSP), UFS, battery gauge, cameras, modem.
- The full iOS user space needs the iOS 16.5 files (`sysimage/fetch.sh`); the current images test
  the boot chain and the kernel only.

## Firmware

```sh
devices/xiaomi-12/pull-firmware.sh      # the phone in Android, on adb
```

The script is the POCO's list adapted to the Xiaomi 12 (Adreno 730 GPU, WCN6855 Wi-Fi, ADSP,
Awinic haptics, modem, cameras); the files are the vendor's and stay out of git.

## Running

From the top of the repository, see the main README: `./xl enter`, `./xl ramdisk`, `./xl run`, or
everything in one command: `DEVICE=xiaomi-12 tools/modem-cycle.sh "ARGS" SECS`.

## Porting notes

The SoC addresses are the phone's own: `soc.args` carries the modem/IPA ones, and the kernel drivers
take them from boot-args. The loader's `platform_cupid` and the UEFI memory map are the two places
that are still per-phone source (as they are for the POCO). The heads of the new drivers
(`hakios.fts.c`, the cupid branch of `hakios.buttons.c`) say where each number came from.
