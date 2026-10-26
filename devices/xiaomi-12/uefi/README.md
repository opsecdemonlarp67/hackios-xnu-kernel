# UEFI for cupid

A port of [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to the
Xiaomi 12 (cupid, SM8450 "waipio"), modelled on `ingres` (the POCO F4 GT, the same SoC). You do not
need to rebuild it to run HakiOS: the built image is in `devices/xiaomi-12/prebuilt`, and
`mkxnuimg.py` takes it.

What is here:

- `BASE`: the Mu-Silicium commit the port is made on;
- `mu-silicium.patch`: changes to existing files: `BootShim/BootShim.S` (copies our "XNUP"
  container behind the UEFI image to 0xF4600000) and `BootManagerLib` (starts the XNU loader from the
  container instead of a menu);
- `files/`: new files: `Platforms/Xiaomi/cupidPkg` (memory map, driver list),
  `Silicon/.../BootManagerLib/XnuLoader.[ch]`, `Resources/Configs/cupid.toml`,
  `Resources/DTBs/cupid.dtb` (the device tree from the phone's vendor_boot), `Binaries/cupid`
  (the UEFI drivers, taken from Project Silicium's `Device-Binaries` for `ingres`, since cupid's own
  `uefi.img` has no extractable EDK2 driver set).

The FD lives at `0xA7000000` in a 4 MB region, as in the phone's own UEFI memory map; the XNUP
payload sits at `0xF4600000` (the start of the second usable DRAM chunk) and the DXE heap was moved
to `0xF8600000` so it starts after the payload. `SimpleFbDxe` hands over the splash framebuffer at
`0xB8000000`; the loader recognises it as cupid.

## Build

Packages as in Mu-Silicium's `setup_env.sh` (Arch: `git base-devel lld nasm clang llvm
aarch64-linux-gnu-gcc python python-pip python-virtualenv`).

```sh
devices/xiaomi-12/uefi/apply.sh       # clones Mu-Silicium (with submodules, ~3 GB) and applies the port
python3 -m venv venv && venv/bin/pip install -r Mu-Silicium/pip-requirements.txt
cd Mu-Silicium
source ../venv/bin/activate
python3 build_uefi.py -d cupid -r RELEASE     # -> Build/cupidPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

The `mu_nasm` dependency comes through NuGet and wants Mono; without Mono, point it at the
system nasm:

```sh
D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p $D/Linux-x86-64
echo 'version: 20016.1.1' > $D/extdep_state.yaml
ln -s /usr/bin/nasm /usr/bin/ndisasm $D/Linux-x86-64/
```

After a build, `mkxnuimg.py` (called by `./xl enter`) takes the UEFI from `Mu-Silicium/`, and
`tools/update-prebuilt.sh` refreshes `devices/xiaomi-12/prebuilt`. `devices/xiaomi-12/uefi/export.sh`
is the reverse: it saves the changes from `Mu-Silicium/` here.
