#!/usr/bin/env python3
"""
Pack a phone's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The device is chosen with DEVICE (default: poco-x5-pro) or --device; its directory
devices/<device>/ holds image.conf (the FD base and size, the payload base, the DTB
name) and prebuilt/ (the UEFI pieces when there is no Mu-Silicium build here).

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see
devices/<device>/uefi/README.md) or, when there is none, from devices/<device>/prebuilt/.

usage: [DEVICE=xiaomi-12] mkxnuimg.py [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
ap.add_argument('--device', default=os.environ.get('DEVICE', 'poco-x5-pro'))
args = ap.parse_args()

DEVICE = args.device
DEVICE_DIR = os.path.join(HERE, 'devices', DEVICE)
PREBUILT = os.path.join(DEVICE_DIR, 'prebuilt')
CONF = os.path.join(DEVICE_DIR, 'image.conf')

# defaults are the POCO X5 Pro's, the first device
conf = {'uefi_pkg': 'redwoodPkg', 'fd_base': '0x9FD00000', 'fd_size': '0x200000',
        'payload_base': '0xF0000000', 'payload_max': '0x04000000',
        'dtb': 'redwood.dtb', 'output': 'xnu-redwood.img'}
if os.path.isfile(CONF):
    for line in open(CONF):
        line = line.split('#', 1)[0].strip()
        if not line or '=' not in line:
            continue
        k, v = line.split('=', 1)
        conf[k.strip()] = v.strip()
FD_BASE, FD_SIZE = int(conf['fd_base'], 0), int(conf['fd_size'], 0)
PAYLOAD_BASE, PAYLOAD_MAX = int(conf['payload_base'], 0), int(conf['payload_max'], 0)
UEFI_PKG = conf['uefi_pkg']
DTB_NAME = conf['dtb']
if args.output is None:
    args.output = os.path.join(HERE, conf['output'])

fd_path = os.path.join(MU, f'Build/{UEFI_PKG}/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, 'Resources/DTBs', DTB_NAME), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
