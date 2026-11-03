# Distro Buny — Architecture

Distro Buny is a UEFI multiboot drive for x86-64 Linux ISO images. A user
copies unmodified `.iso` files into folders on the drive; at boot, Distro Buny
shows them in a graphical menu and starts the selected system. Distro Buny is
an original implementation. It does not contain, wrap or call Ventoy,
Easy2Boot, YUMI or any other multiboot product. It uses general-purpose
building blocks (shim, GRUB, the Linux kernel) only for the parts of the boot
chain that the UEFI Secure Boot trust model requires.

This document describes how the system is built. `REPORT.md` covers what was
verified and how.

## 1. The big picture

```
 power on
    │
    ▼
 UEFI firmware ──► \EFI\BOOT\BOOTX64.EFI  shim 15.8 (Microsoft-signed)
                         │ verifies
                         ▼
                   grubx64.efi            GRUB 2.12 (Canonical-signed)
                         │ reads EFI/BOOT/grub.cfg (generated), verifies kernel via shim
                         ▼
                   /distrobuny/vmlinuz    Linux 7.0 (Canonical-signed)
                   /distrobuny/initramfs.img  ── the Distro Buny runtime
                         │
     ┌───────────────────┴──────────────────────────────────────┐
     │ buny-init (PID 1, root)          buny-ui (uid 100)        │
     │  • drivers, drive discovery       • graphical menu        │
     │  • mounts ESP + data (read-only)  • ISO catalog/probing   │
     │  • validates boot requests  ◄─IPC─► • boot planning       │
     │  • kexec_file_load + reboot      • reads kernel/initrd    │
     └───────────────────┬──────────────────────────────────────┘
                         │ kexec (no firmware round-trip)
                         ▼
                   distribution kernel + [distribution initrd ⊕ Distro Buny overlay]
                         │ rdinit=/distrobuny/preinit
                         ▼
                   preinit: maps the ISO's bytes on the data partition to
                   /dev/loop200 (read-only), then the distribution's own init
                   finds "its CD" there and boots normally
```

The design has three ideas:

1. **The menu is a real Linux program, not a bootloader script.** A small
   Linux runtime (signed kernel + custom initramfs) starts in seconds and
   gives the interface proper graphics (DRM/KMS), USB keyboard/mouse/touchpad
   input, real file systems and threads.
2. **Distributions start through kexec.** The runtime reads the chosen
   distribution's kernel and initrd straight out of the ISO and starts them
   with `kexec_file_load`. The firmware is never involved again, so a
   distribution starts the same way on every PC.
3. **The ISO becomes a disc again (the "virtual medium").** Live and
   installer initrds expect to find their medium as a block device: a CD, a
   USB stick or a partition carrying the ISO's file system. Distro Buny adds
   a tiny pre-init program to the distribution's initrd. It exposes the exact
   byte range of the ISO file on the data partition as a read-only loop
   device, `/dev/loop200`. To the distribution this looks like its original
   medium, so most distributions need no ISO-file boot support of their own.

## 2. Drive layout

```
LBA 0         protective MBR
LBA 1-33      primary GPT
1 MiB         partition 2  "BUNY-EFI"    FAT32   512 MiB EFI System Partition
513 MiB       partition 1  "DISTROBUNY"  exFAT   ISO collection (rest of the drive)
end           backup GPT
```

* **Data partition: exFAT, first in the table, last on the disk.** Windows,
  macOS and Linux can all read and write exFAT natively. It has no 4 GiB
  file-size limit (DVD and gaming ISOs exceed it) and no journal to replay.
  Some operating systems only mount the first partition of removable media,
  so DISTROBUNY is entry 1 of the GPT. On the disk it comes last, so that it
  can grow to the end of a bigger stick (below). The partition fills the
  drive, so capacity scales with the stick (the reference image holds 45
  ISOs, 169.7 GiB).
* **Contiguous files.** The image builder allocates every file contiguously
  and marks it `NoFatChain`, so an ISO is one extent on the partition, which
  is cheap to map as a loop device. Files that users later add or copy are
  often contiguous too. When they are not, the runtime maps the fragments with
  a device-mapper linear table, or refuses with a clear message.
* **ESP first on the disk, FAT32, 512 MiB.** It holds shim, GRUB, the MOK
  manager, the runtime kernel and initramfs, `grub.cfg`, optional per-drive
  options and the runtime's small state files (`distrobuny/state/`).
* **Hidden metadata.** `/.distrobuny/` on the data partition holds
  `volume.json` (build identity), `index.json` (pre-computed catalog so the
  menu appears instantly), the updater's `staging/` area and `updates.log`.

### Growing the data partition (images written to bigger sticks)

balenaEtcher, Rufus and `dd` copy an image byte for byte, so on a bigger
stick DISTROBUNY ends where the image ended and the backup GPT sits in the
middle of the stick. Three ways to use the rest were compared:

| Option | Verdict |
| --- | --- |
| (a) Grow the early 1.0.0 layout in the runtime: move the ESP to the end, then grow exFAT | Rejected. Its FAT region (1-4 MiB) holds at most 786,430 entries, so with 32 KiB clusters the volume can never pass 24 GiB, even after the ESP has moved. Its bitmap would also have to move (a single cluster covers 8 GiB). Moving the boot partition of the running drive adds risk for no gain. |
| (b) ESP first, growable exFAT last, grown by the runtime | **Chosen.** Growing rewrites only metadata: no file data or partition moves. |
| (c) Starter variants sized for 8/16/32/64 GB sticks | Rejected as the answer. Users must pick the right file, sticks of one label differ in size (a "64 GB" stick has 58-60 GiB), and flashing writes the whole image (64 GB at USB speed takes about an hour). Still possible with `buny-build image --profile starter --size-gib N` if a fixed size is wanted. |

Images from `buny-build image` (all profiles) are therefore *growable*:
128 KiB clusters, room for a 2 TiB FAT reserved between the FAT and the
cluster heap (64 MiB), and the allocation bitmap in the last clusters of the
heap, exactly where new clusters are appended. Both are allowed by the exFAT
specification; the volume passes `fsck.exfat` as it is. `buny-install create`
builds drives that already fill their disk and uses the plain layout.

`source/filesystem/expand.c` grows such a drive. buny-init plans at
start-up (runtime.json `expand`); the menu offers *Use the whole drive* and
asks for confirmation; init then unmounts DISTROBUNY and runs the steps
below, resizes the kernel's partition (`BLKPG`) and mounts it again:

1. GPT: the backup copy at the new end of the disk, then the primary
   entries, then the primary header; then the protective MBR's size.
2. exFAT, with `VolumeDirty` set: FAT entries and bitmap bits of the new
   clusters, the bitmap's FAT link and directory entry (`DataLength`), the
   backup boot region, then the main boot region with the new size and
   `VolumeDirty` cleared, in one 6 KiB write.

Every write is flushed before the next one. Every step is idempotent and
derived from what is on the disk. Until the primary GPT header is written,
the old table is valid. A partition larger than its file system is valid.
A torn primary GPT or main boot region fails its CRC or checksum, and the
already-new backup copy describes the result. `expand_plan()` recognises
the interrupted states (a damaged primary GPT or main boot region, or a
partition already larger than its volume), and buny-init finishes them at
the next start before mounting. UEFI firmware may get there first: OVMF
(EDK2) rebuilt a torn primary GPT from the new backup during the power-cut
test, which leaves an enlarged partition around the old volume. It refuses, with a reason the menu shows, when:

* the ESP lies behind DISTROBUNY (the layout of early 1.0.0 builds);
* the volume is dirty without an expansion in progress (unsafe removal);
* the file system is not exFAT, damaged, or its bitmap is not where it
  can grow;
* the drive has 4 KiB logical sectors or is write-protected.

A plain (non-growable) volume grows only as far as its FAT and bitmap
allow. `tests/unit/python/test_image.py` (`ExpandTest`) cuts the power
after every single write and checks that finishing gives a byte-identical
result. `tests/boot/test_starter_image.py` does the same in a VM through
the menu, a restart and power cuts, with `fsck.exfat`/`fsck.fat` on the
VM's disk.

The images are written by Distro Buny's own deterministic writers
(`source/python/distrobuny/image/`: GPT, FAT32 with long names, exFAT with
up-case table, allocation bitmap and checksummed entry sets). Identical inputs
give a bit-identical image (verified by `tests/unit/python/test_image.py`). The
results are checked by the reference `fsck.fat` and `fsck.exfat` and by the
runtime's independent C exFAT reader.

## 3. Boot chain and the runtime

### 3.1 Firmware to runtime

* `EFI/BOOT/BOOTX64.EFI` is shim 15.8 (`shimx64.efi.signed.latest` from
  Ubuntu's `shim-signed 1.58`), signed by Microsoft's UEFI CA. It is accepted
  by any PC that ships with Secure Boot enabled.
* shim verifies and starts `grubx64.efi` (Ubuntu `grub-efi-amd64-signed
  1.202.5+2.12-1ubuntu7.3`), signed by Canonical. GRUB's built-in prefix is
  `/EFI/ubuntu`, so the generated `grub.cfg` is installed in both
  `EFI/ubuntu/` and `EFI/BOOT/`.
* `grub.cfg` (generated by `source/python/distrobuny/build/esp.py`) waits
  invisibly for 1 second (Esc shows the menu) and records the ESP's partition
  GUID (`probe --part-uuid`) so the runtime can find its own drive. It reads
  optional per-drive settings from `/distrobuny/options.cfg` and loads the
  runtime kernel and initramfs. Advanced entries: diagnostic mode, an
  emergency shell, the MOK manager, firmware settings, restart.
* The kernel is Ubuntu's Canonical-signed `7.0.0-38-generic`. GRUB verifies
  it through shim's protocol, so the whole chain up to the Distro Buny
  runtime is signed. Distro Buny itself has no Secure Boot key; the
  consequences are discussed in `SECURITY.md`.

All boot-chain packages are pinned by version and SHA-256 in
`configuration/boot/components.json` and verified before use.

### 3.2 The runtime (initramfs)

The initramfs (about 20.7 MiB, zstd) is built by
`source/python/distrobuny/build/runtime.py`. It contains:

| Component | Role |
| --- | --- |
| `buny-init` (static C, PID 1) | privileged half: hardware, drives, mounts, validation, kexec |
| `buny-ui` (static C) | unprivileged half: the graphical menu |
| `buny-preinit` (static C) | placed into distribution initrds at boot time (virtual medium) |
| 1074 kernel modules | storage (USB, NVMe, SATA, MMC), input (HID, I2C touchpads), DRM, loop, dm, exfat/vfat, qemu_fw_cfg |
| BusyBox | emergency shell and diagnostics only |
| fonts, logo, icons, `distributions.conf`, `compatibility.json` | interface assets and identification rules |

**`buny-init`** (`source/boot/init.c`) mounts the kernel file systems,
including securityfs (for the lockdown state) and efivarfs. It loads drivers
for every device present: it reads modaliases, resolves them through
`modules.alias` and `modules.dep`, and loads them with `finit_module`. It
keeps handling hot-plug uevents, so a keyboard plugged in later still works.
It then finds the ESP by the partition GUID passed from GRUB and the data
partition on the same disk by GPT name `DISTROBUNY`, falling back to the file
system label. It mounts the data partition read-only and starts the
interface as user 100 with `PR_SET_NO_NEW_PRIVS`. After that it serves the
interface's requests over a `SOCK_SEQPACKET` socket pair:

| Request | What init does |
| --- | --- |
| `IPC_BOOT` (+ kernel and initrd as memfds) | checks the path stays inside the data partition (`realpath`), maps the ISO's extents, verifies the medium, builds the pre-init overlay, calls `kexec_file_load` |
| `IPC_EXECUTE` | unmounts, syncs, `reboot(LINUX_REBOOT_CMD_KEXEC)` |
| `IPC_POWER` | restart / power off / restart into firmware setup (sets `EFI_OS_INDICATIONS_BOOT_TO_FW_UI`) |
| `IPC_SAVE` | writes a whitelisted state file (`settings.json`, `last-boot.json`) to the ESP |
| `IPC_RESCAN` | looks for the data partition again (drive inserted late) |

**`buny-ui`** (`source/interface/`) is described in section 6. It can read
the data partition (mounted read-only) but cannot write to any disk, load
modules or call kexec. The interface parses all untrusted input: ISO file
systems, GRUB scripts and distribution configuration. Only small, validated
requests reach the privileged process.

### 3.3 Hand-off to a distribution

1. The interface opens the ISO with Distro Buny's ISO 9660 reader and asks
   the adapter layer for a **boot plan** (section 5): which kernel, which
   initrd(s), which kernel parameters, and how the system will find its
   medium.
2. It copies the kernel and initrd(s) out of the ISO into memfds and sends
   them to init with `IPC_BOOT`, along with the path, the ISO's volume label
   and the command line.
3. For the virtual medium, init computes where the ISO's bytes lie on the
   partition. For exFAT it reads the directory entries directly
   (`source/filesystem/exfat.c`); other file systems use FIEMAP. It then
   verifies the mapping: the volume label in the ISO's primary volume
   descriptor and the first and last 64 KiB read through the map must equal
   the file's contents. Next it writes a cpio overlay containing
   `/distrobuny/preinit` and `medium.conf` (partition start and size, file
   system serial, extents, ISO size and label), appends it to the
   distribution's initrd (initrds may be concatenated) and adds
   `rdinit=/distrobuny/preinit`, chaining to the original `rdinit` or `/init`.
4. `kexec_file_load` loads kernel, initrd and command line. Under Secure Boot
   the kernel's lockdown checks the distribution kernel's signature here
   (section 7 of `SECURITY.md`). Errors are translated into plain language,
   for example "Secure Boot is on and this system's kernel is not signed".
5. The interface records the last started system, then asks init to execute.
   init unmounts and syncs, and the machine jumps into the distribution
   kernel.

### 3.4 The pre-init (virtual medium)

`buny-preinit` (`source/boot/preinit.c`) is a static, dependency-free
program. It runs as PID 1 of the distribution's initrd for a few
milliseconds:

* It forks. The parent immediately `exec`s the distribution's real init, so
  the distribution's boot is not delayed or changed. The child (argv[0]
  starting with `@`, so systemd's shutdown "killing spree" leaves it alone)
  does the work in a private mount namespace, with its own devtmpfs and sysfs.
* It waits until the data partition appears. It identifies the partition by
  start sector, size and file-system serial, not by name, so it does not
  depend on device enumeration order.
* It makes sure the loop driver is available. The driver may be built in,
  in the distribution's initrd, loadable with the distribution's `modprobe`,
  or found in `/lib/modules`.
* It creates `/dev/loop200` with `LOOP_CONFIGURE`, read-only, with the ISO's
  offset and size limit. Fragmented files get a device-mapper linear target
  instead. Finally it reads the primary volume descriptor through the new
  device and checks the volume label before announcing success in the kernel
  log.
* For the Debian installer, whose initrd has no loop driver, the plan adds
  the image's own `loop-modules` udeb and a udev rule that tags
  `/dev/loop200` as a CD drive. The pre-init extracts the driver with the
  installer's BusyBox into `/distrobuny/modules` (never into `/`, whose
  `lib` is a link to `usr/lib`) and loads it from there.

The distribution's initrd then looks for its medium in the usual way. The
adapter has already added the parameter that makes it look at
`/dev/loop200` (or at the label or UUID the loop device now carries).

**Native ISO-file boot** is the alternative strategy. Some distributions
can find an ISO file on a partition themselves (`iso-scan/filename`,
`img_dev`/`img_loop`, `findiso`, `inst.stage2=hd:…:/path`). For those the
plan can pass the file path instead of adding the pre-init. Paths containing
spaces or other characters that these mechanisms cannot express are
detected, and the plan falls back to the virtual medium.

## 4. ISO discovery and the catalog

`source/iso-manager/catalog.c` scans the data partition: every `*.iso` below
the root, skipping hidden folders such as `.distrobuny`. Depth and node
limits and inode loop detection protect it from pathological trees. The
**folder tree is the menu**: top-level folders become categories (`Desktop`,
`Servers`, plus any the user creates), and sub-folders become nested pages
with breadcrumbs and Back navigation. Folder rules
(`configuration/distributions/distributions.conf`, `[folder:*]`) supply
icons and descriptions. They replace a folder's name only when the rule
matches the whole name ("open suse" → "openSUSE"); otherwise the user's name
stays ("Ubuntu Family").

For each ISO the catalog stores metadata from a **probe**
(`source/iso-manager/identify.c`):

* **ISO 9660 reader** (`source/filesystem/iso9660.c`): primary volume
  descriptor, Rock Ridge (long names, symlinks, continuation areas), Joliet,
  multi-extent files and a case-insensitive fallback. It uses sector and
  directory caches, and lookups return owned copies so callers never hold
  cache pointers.
* **Boot configuration parsers**: a sandboxed GRUB script interpreter
  (`source/boot-adapters/grubcfg.c`). It handles variables, quoting, word
  splitting, `if/elif`, `for` with globbing over the ISO, `while/until`,
  functions, `menuentry/submenu`, `source/configfile`,
  `search/probe/regexp/test`, and a step limit. The interpreter evaluates the
  ISO's `grub.cfg` and `loopback.cfg` the way GRUB would, but against the ISO
  file system and never with side effects. There are also parsers for
  systemd-boot entries, SYSLINUX/ISOLINUX configuration, `.treeinfo`,
  `.disk/info` and similar identity files.
* **Identification rules** (62 distribution sections and 12 folder rules in `distributions.conf`, POSIX
  regular expressions over label, file name, `.disk/info`, kernel version,
  etc.) yield the distribution name, version, edition and icon.
* **Kernel inspection**: the bzImage header (architecture, version string)
  and the Authenticode signature (signed or not, signer's common name, for
  example "Canonical Ltd. Secure Boot Signing (2022 v1) (issued by Canonical
  Ltd. Master Certificate Authority)").

The build writes the catalog into `/.distrobuny/index.json`. At boot the
menu restores metadata for every ISO whose size and modification time still
match, so 45 ISOs appear with full details about 0.15 s after the menu starts (measured on the released image). New or changed
files are probed in worker threads while the menu is already usable.

## 5. Distribution adapters

An adapter (`source/boot-adapters/adapters.c`) knows how one family of
initrds finds its medium. Each adapter provides `detect()`, which scores how
well an ISO matches, plus `prepare_virtual()` and `prepare_native()`, which
adjust a boot entry for each strategy or explain why it is impossible.

| Adapter | Families | Virtual medium | Native ISO file |
| --- | --- | --- | --- |
| `casper` | Ubuntu and flavours, Mint, Pop!\_OS, Zorin, elementary | `live-media=/dev/loop200` | `iso-scan/filename=` |
| `archiso` | Arch, EndeavourOS, CachyOS, Omarchy, BlackArch | label/UUID search finds the loop device (`archisosearchuuid`, `rootdelay`); label-only entries of older archiso (BlackArch 2023) get `archisodevice=/dev/loop200` | `img_dev=` + `img_loop=` |
| `miso` | Manjaro | `misolabel` finds the loop device | `img_dev=` + `img_loop=` |
| `dracut-live` | Fedora live, Bazzite, Nobara, Void, Gentoo | `root=live:CDLABEL=` finds the loop device | `iso-scan/filename=` |
| `anaconda` | Fedora Atomic/IoT/Server installers, AlmaLinux, CentOS Stream (RHEL, Rocky) | the ISO's own `inst.stage2=hd:LABEL=` (the loop device carries the label) | `inst.stage2=hd:UUID=<data>:/path` |
| `kiwi` | openSUSE Leap installer, Leap Micro | `root=install:CDLABEL=` finds the loop device | `iso-scan/filename=` |
| `linuxrc` | openSUSE Tumbleweed/MicroOS DVD | `install=hd:/?device=loop200` | not implemented |
| `debian-live` | Debian live, Kali Linux, Parrot OS | `live-media=/dev/loop200` | `findiso=` |
| `nixos` | NixOS | `root=LABEL=` finds the loop device | `findiso=` |
| `guix` | GNU Guix System | `root=` file-system UUID on the loop device | none (the Guix initrd has no ISO-file option) |
| `antix` | antiX, MX Linux | `bdev=/dev/loop200` (live-init's own scan skips loop devices; `from=` removed) | not implemented |
| `slackware` | the Slackware installer | the installer runs from its initrd; `setup` is given `/dev/loop200` as a custom CD/DVD device (the menu says so) | none |
| `debian-installer` | Debian netinst/DVD | the image's own `loop-modules-<release>-di` udeb is extracted by the pre-init; a udev rule makes `/dev/loop200` a CD drive, which `cdrom-detect` mounts | blocked (the CD initrd has no iso-scan) |
| `generic-loopback` | anything with a `loopback.cfg` using `iso_path` | — | as the ISO's own `loopback.cfg` says |
| `best-effort` | any other 64-bit Linux image with a kernel and initrd (antiX/MX, Alpine, Puppy, Slax, ...) | the image's own entry, unchanged; the loop device carries its label and UUID | — |

Families are recognised by how they boot (kernel arguments, file layout),
never by name, so derivatives work without changes (Kali, Tails, Parrot,
Clonezilla and GParted use `debian-live`; Rocky and Alma `anaconda`;
SystemRescue `archiso`; Garuda `miso`). `best-effort` scores lowest, so any
adapter that knows the family wins. The menu marks such images *Best effort*
and asks before starting one.

The planner (`source/boot-adapters/plan.c`) ranks boot entries. Entries for
rescue, media checks, accessibility, network boot and memory tests are
deprioritised or excluded. The planner picks a strategy (the user can force
one under Boot options) and validates that every referenced kernel and initrd
exists in the ISO. The result is graded by honest levels: *recognised →
method known → prepared → (verified in the lab)*. A plan can also carry a
**warning** for destructive behaviour. Self-installing KIWI images that
overwrite a disk immediately get an "Erases a disk" badge and need an
explicit confirmation before they start.

Adding a family means writing a detect function and one or two small
`prepare` functions, plus identification rules. `DEVELOPER_GUIDE.md` has a
walkthrough.

## 6. The graphical interface

`source/interface/` is a self-contained C program with a software renderer:

* **Display** (`display.c`): DRM/KMS dumb buffers with dirty-rectangle
  flushing, a fbdev fallback, and an offscreen backend for tests and
  screenshots (PNG writer).
* **Graphics** (`gfx.c`, `path.c`): a premultiplied BGRA canvas,
  signed-distance rounded rectangles with analytic anti-aliasing, cached soft
  shadows, an SVG-path rasterizer with exact area coverage (used for the
  distribution marks), and dithered gradients without banding.
* **Text** (`font.c`): stb_truetype with a glyph cache, 4× sub-pixel
  positioning, gamma-corrected coverage, ellipsizing and wrapping. The UI
  font is Inter.
* **Input** (`input.c`): evdev keyboards, mice, touchpads (tap-to-click and
  two-finger scrolling) and absolute pointers such as tablets and VMs. inotify
  provides hot-plug.
* **Application** (`app.c`, `render.c`): a screen stack (home, folder,
  detail, boot progress, settings, information, search, log, no-drive),
  dialogs, spatial keyboard focus, hover and press feedback, animated
  transitions, light and dark themes, and automatic scaling from 800×600 to
  4K. Probing and booting run on worker threads, and an eventfd wakes the
  render loop.

The home screen shows the categories and a "start last system" shortcut. A
folder page lists sub-folders and ISOs as cards with the distribution mark,
version, size, architecture and a compatibility badge (*Verified*, *Ready*,
*Best effort*, *Needs Secure Boot off*, *Erases a disk*, *Known issue*,
*Not supported*, *Damaged*, *Unreadable*, *Checking…*). The detail page shows image facts (version, edition, kernel,
label, kernel signature, boot method, media strategy, lab-test record), the
start button and boot options: entry variant, safe graphics (`nomodeset`)
and media strategy.

Branding: the Distro Buny mark is generated from the supplied logo
(`Distro buny logo.png`) by `build/assets.py`. Distribution marks are
vector paths from Simple Icons (CC0) with per-brand colours. Families
without a Simple Icons mark get a neutral monogram (see `assets/` for
licences and the trademark disclaimer).

## 7. Host tools

| Tool | Purpose |
| --- | --- |
| `tools/build/buny-build` | `doctor`, `runtime`, `esp`, `image --profile full\|dev\|lab`, `refresh-esp` |
| `tools/maintenance/buny-install` | `list`, `write` (image → drive), `create` (ISO folder → drive) with drive-safety rules |
| `tools/maintenance/buny-update` | `sources`, `status`, `check`, `update`: verified ISO updates |
| `tools/development/buny-test-matrix` | boots every image on a drive in QEMU/OVMF and records evidence |
| `tools/development/buny-compat` | turns test runs into the menu's compatibility records |
| `build/bin/bunyctl` | `probe`, `scan`, `extract`, `adapters`, `stage`, `map`: the C core from the command line |

The Python host tools use only the standard library (plus `gpg` for the
updater).

## 8. Updates

`tools/maintenance/buny-update` (`source/python/distrobuny/updater/`) keeps
ISOs current without trusting the network:

1. **Inventory**: `bunyctl scan` identifies every ISO, and
   `configuration/updates/sources.json` (22 sources) maps it to a vendor
   source with a file-name pattern.
2. **Check**: downloads the vendor's checksum list over HTTPS and verifies
   its OpenPGP signature in a throw-away keyring. The key must be the
   vendor's, pinned by fingerprint in `configuration/updates/keys/`, and both
   detached and clear-signed formats are supported. Only then are versions
   compared (LTS series stay on LTS).
3. **Download**: resumable (HTTP Range) into `/.distrobuny/staging/` on the
   drive itself, with space reserved first. The checksum is verified before
   anything else happens.
4. **Apply**: only after approval (prompt or `--yes`). The verified file is
   moved into place atomically next to the old one, the old image is removed
   only afterwards (`--keep-old` keeps it), `updates.log` records the change
   and the drive's index is refreshed. Any failure leaves the original image
   untouched.

## 9. Installing onto a drive

`tools/maintenance/buny-install` (`source/python/distrobuny/install/`):

* **Drive discovery and assessment** read sysfs and procfs. Mounts are
  traced through device-mapper and RAID stacking (`slaves/`), plus swap and
  holders. A drive holding the running system is never a valid target. A
  drive with mounted or held file systems, write protection, 4K logical
  sectors, too little capacity or no medium is refused. A fixed (non-USB/SD)
  disk needs `--allow-fixed-disk`.
* The drive is opened with `O_EXCL`, an exclusive kernel claim that fails
  while anything is mounted and blocks mounting while held. Its identity
  (device number, size, sector size, model, serial) is re-checked after
  opening. The user must type the drive name to confirm.
* **Writing** is planned as non-overlapping segments. Zones where stale
  partition tables or file-system signatures could survive (first and last
  MiB of the disk, first 8 MiB of each partition) are zeroed explicitly. The
  partition table is written last, with fresh GUIDs and the backup at the
  drive's real end, and everything written is read back and compared.
  `create` mode builds the volume directly on the drive, sized to the whole
  drive, without an intermediate image.
