# Distro Buny — Build Instructions

Everything is built inside the project folder without root privileges. Only
writing a physical USB drive (`buny-install`) needs root, and that step is
always explicit.

## 1. Host requirements

Tested on Ubuntu 24.04 (x86-64, Linux 7.0). Any recent Debian/Ubuntu-like
system works. Other distributions work if they provide `dpkg-deb` (used to
unpack the pinned Ubuntu boot packages).

| Package (Ubuntu name) | Used for |
| --- | --- |
| `build-essential` (gcc, make) and `libc6-dev` (static `libc.a`) | interface, runtime, `bunyctl` |
| `python3` (3.10 or newer) | build pipeline, installer, updater, tests (standard library only) |
| `zstd`, `cpio` | runtime initramfs |
| `dpkg` (`dpkg-deb`) | unpacking the pinned boot packages |
| `gnupg` | updater signature checks |
| `qemu-system-x86`, `ovmf` | virtual-machine tests |
| `genisoimage`, `dosfstools` | tests (ISO fixtures, reference `fsck.fat`) |
| `sbsigntool`, `openssl` | optional: own Secure Boot signing experiments |

`/dev/kvm` access (membership in the `kvm` group) makes VM tests about 20×
faster.

Check everything at once:

```bash
tools/build/buny-build doctor
```

## 2. Build steps

```bash
cd "Distro buny"

make -j"$(nproc)"                        # host tools: build/bin/bunyctl, build/bin/buny-ui-host
tools/build/buny-build runtime           # signed kernel + initramfs  → build/runtime/
tools/build/buny-build esp               # shim, GRUB, grub.cfg, runtime → build/esp/
tools/build/buny-build image --profile full    # → output/images/distrobuny-1.0.1-full.img
```

* `runtime` downloads the pinned Ubuntu packages listed in
  `configuration/boot/components.json` (shim-signed, grub-efi-amd64-signed,
  linux-image, linux-modules) into `build/cache/debs/`, verifies their
  SHA-256, and builds the static runtime programs (`make runtime`). It then
  selects kernel modules and writes `build/runtime/initramfs.img`.
* `esp` assembles the EFI System Partition tree in `build/esp/`.
* `image` catalogs the ISO collection (`Iso layout/` by default, `--isos DIR`
  otherwise), writes `/.distrobuny/index.json` and `volume.json`, and builds
  the disk image with Distro Buny's own GPT/FAT32/exFAT writers. It also
  writes a manifest (`output/manifests/<image>.json`: geometry, file systems,
  every file's offset) and a checksum (`output/checksums/<image>.sha256`).

Image profiles:

| Profile | Content | Typical size |
| --- | --- | --- |
| `full` | every ISO in the collection | 178.3 GiB for the reference collection |
| `dev` | a few small ISOs for quick VM tests | 9.4 GiB |
| `lab` | one ISO per family | — |
| `starter` | no ISOs, empty `Desktop` and `Servers`: the public download (section 9) | 3.5 GiB, 39 MiB as `.img.xz` |
| `--include REGEX` | any selection (repeatable) | — |

Images are sparse files: unused space takes no room on the host.
`--size-gib N` builds an image of exactly N GiB whose data partition fills
it (the `starter` profile uses 3.5, which fits every "4 GB" stick; it ignores
`--isos`).

### Fast iteration on the boot chain

After changing the runtime or interface, rebuild and rewrite only the ESP of
an existing image (seconds instead of minutes). The image's checksum and
manifest are updated as well:

```bash
tools/build/buny-build runtime && tools/build/buny-build esp
tools/build/buny-build refresh-esp --image output/images/distrobuny-1.0.1-dev.img
```

### Reproducible builds

Set `SOURCE_DATE_EPOCH` to fix all timestamps. With identical inputs (ISO
files, pinned packages, source tree) the image is bit-identical. Partition
GUIDs and file-system serials are derived from the build identifier, and
allocation follows the natural sort order of names. The unit test
`test_reproducible` checks this.

```bash
SOURCE_DATE_EPOCH=1792713600 tools/build/buny-build image --profile full
```

The released reference image was built this way.

## 3. Trying an image in a virtual machine

```bash
tools/development/buny-vm                       # full image (or the dev image), UEFI
tools/development/buny-vm --secure-boot         # with Secure Boot enforced
tools/development/buny-vm --image path/to.img   # any Distro Buny image or stand-in drive
tools/development/buny-vm --image output/images/distrobuny-1.0.1-starter.img --stick-gib 16
                                                # the image on a 16 GiB stick, as Etcher/Rufus leave it
                                                # (the menu offers "Use the whole drive")
```

A QEMU window opens with UEFI firmware and the image attached as a USB stick.
The VM writes to a throw-away overlay, so the image never changes. Ctrl+Alt+G
releases a grabbed keyboard; closing the window stops the VM.

## 4. Running the menu without a VM

The interface runs on a normal desktop as an offscreen program for
development and screenshots:

```bash
BUNY_DISPLAY=offscreen:1920x1080 BUNY_DATA="Iso layout" \
BUNY_SHARE=build/runtime-root/usr/share/distrobuny \
BUNY_SCRIPT="wait:800;shot:/tmp/home.png;key:Enter;wait:400;shot:/tmp/desktop.png;quit" \
build/bin/buny-ui-host
```

Script commands: `key:NAME`, `type:TEXT`, `move:X,Y`, `click:X,Y`,
`clickon:PATH|@back|@boot|@search|@settings|@crumb:N`, `scroll:DY`, `wait:MS`,
`shot:FILE`, `quit`. Without the privileged runtime the program can only
prepare a boot (dry run); it never starts a kernel.

## 5. Tests

```bash
tools/build/buny-build test-tools            # pinned reference fsck.exfat + mtools (once)
make test                                    # C unit tests
python3 tests/unit/python/test_image.py      # image writers vs fsck.fat / fsck.exfat / C reader, reproducibility
python3 tests/unit/python/test_install.py    # drive safety rules, installer writing and verification
python3 tests/unit/python/test_updater.py    # updater against a local HTTPS-like server and throw-away keys
python3 tests/integration/test_ui_navigation.py   # the real menu driven by keyboard and mouse (and an empty drive)
python3 tests/boot/test_starter_image.py     # the starter image in a VM: empty, on a bigger stick, ISO added later,
                                             # expanded from the menu, power cuts while expanding, old layout refused
```

Virtual-machine tests boot images in QEMU with OVMF. VMs only ever write to
throw-away snapshots, so the image is never modified:

```bash
# every ISO on an image, two VMs at a time
tools/development/buny-test-matrix --image output/images/distrobuny-1.0.1-dev.img --jobs 2
# a subset, with Secure Boot enforced (Microsoft keys enrolled)
tools/development/buny-test-matrix --image output/images/distrobuny-1.0.1-full.img --filter ubuntu --secure-boot
# turn results into the menu's "Verified" badges
tools/development/buny-compat tests/compatibility/runs/<run> [<newer run>...]
```

Each run directory holds per-image serial logs, screenshots, QEMU command
lines, `result.json`, plus `results.json` and `RESULTS.md`.

## 6. Writing a USB drive

See `USER_GUIDE.md` section 2. In short:

```bash
tools/maintenance/buny-install list
sudo tools/maintenance/buny-install create --isos "Iso layout" --target sdX
```

For testing without hardware, write into a file that stands in for a drive,
then boot that file in QEMU:

```bash
tools/maintenance/buny-install create --isos "Iso layout" --include 'archlinux' \
    --target-file build/vm/usb.img --size 16G
tools/development/buny-test-matrix --image build/vm/usb.img
```

## 7. Output layout

```
build/                  everything intermediate (safe to delete)
  bin/                  bunyctl, buny-ui-host
  runtime-bin/          static runtime programs
  runtime/              vmlinuz, initramfs.img
  esp/                  ESP tree
  cache/debs/           verified upstream packages
  test-artifacts/       screenshots from the interface tests
output/
  images/               disk images
  manifests/            image manifests (JSON)
  checksums/            SHA-256 files
  release/              the publishable download (section 9)
tests/compatibility/runs/   VM test evidence
```

## 8. Updating the pinned boot packages

1. Pick new versions of all packages in `configuration/boot/components.json`
   (shim-signed, grub-efi-amd64-signed, linux-image and linux-modules must
   match each other; `kernel_release` must match the kernel).
2. Put the new paths and SHA-256 values from the archive's signed `Packages`
   index into the file, and update each package's `source` entry (the Ubuntu
   source packages and versions it was built from; `buny-build release`
   checks them against the `.deb` files). The licence notices on the drive
   and in the release are generated from these entries.
3. Rebuild (`runtime`, `esp`, `image`) and re-run the VM tests, including a
   `--secure-boot` run.

## 9. Publishing a release

Only the **starter image** may be published. The `full`, `dev` and `lab`
images contain third-party ISO files, which belong to their publishers, are
far too large to download, and carry the publishers' trademarks. `buny-build
release` refuses any image that is not a starter image or that lists an ISO
file, and any image built with lab automation.

```bash
export SOURCE_DATE_EPOCH=1792713600
tools/build/buny-build sources                                    # once: Ubuntu source packages → build/third-party-sources/
tools/build/buny-build runtime && tools/build/buny-build esp      # no --lab
tools/build/buny-build image --profile starter                    # → output/images/distrobuny-1.0.1-starter.img
python3 tests/boot/test_starter_image.py
tools/build/buny-build release                                    # → output/release/ (verified)
tools/build/buny-build publish --to "/path/to/folder"             # copy, then verify the copy (refuses foreign files)
tools/build/buny-build verify-release "/path/to/folder"           # check any release folder again
```

`output/release/` then holds everything for the download page, as one unit:

| File | Contents |
| --- | --- |
| `distrobuny-1.0.1-starter.img.xz` | the starter image, xz-compressed; balenaEtcher and Rufus open it directly |
| `SHA256SUMS` | SHA-256 of every file in the folder (relative paths) and of the unpacked `.img` |
| `Distro Buny 1.0.1 - User Guide.pdf` | rendered by LibreOffice from `documentation/release/user-guide.html` |
| `Distro Buny 1.0.1 - How it was made.pdf` | rendered from `documentation/release/how-it-was-made.html` |
| `Branding/` | the Distro Buny logo (`Distro buny logo.png`), the mark (`assets/branding/distrobuny-mark.png`) and the 1.0.1 trailer (`media/`) |
| `Licenses and source code/SOURCE-CODE.txt` | the GPL/LGPL source notice: every shipped component, its licence, the exact Ubuntu source packages and where they are in the folder |
| `Licenses and source code/third-party/` | the complete Ubuntu source packages (`.dsc`, upstream tarball, Ubuntu changes) of every shipped binary, about 280 MB |
| `Licenses and source code/distrobuny-1.0.1-source.tar.xz` | Distro Buny's own source (GPL-3.0-or-later), reproducible; without build products and VM test logs |
| `Licenses and source code/LICENSES.txt`, `licenses/` | the notices and licence texts, copied out of the image's own boot partition |

Because the complete corresponding source travels with the image, no written
offer (and no contact address) is needed. Whoever mirrors the image should
mirror the whole folder.

`buny-build release` ends with `verify_release()` (also run by `publish` on
the copy, and by `verify-release`), which refuses the folder when:

* a file is missing from `SHA256SUMS`, or listed there with a wrong hash; the
  unpacked image is checked by decompressing the `.img.xz`;
* `SOURCE-CODE.txt` or `LICENSES.txt` quote a SHA-256 that matches no file of
  the release, or contain unfinished text (placeholders);
* a third-party source package is incomplete (checked against its `.dsc`).

`tests/unit/python/test_release.py` keeps these checks honest, including the
stale source checksum that the first exported 1.0.0 folder had.
