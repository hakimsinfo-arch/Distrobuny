# Distro Buny 1.0.0 — Engineering Report

> Historical: this report describes the internal 1.0.0 build. For 1.0.1 see
> `TEST_RESULTS.md`, `COMPATIBILITY.md` and `CHANGELOG.md` (for example, the
> Debian installer, blocked here, starts since the 1.0.1 fixes).

23 October 2026

This report describes what was built, how it works, and how far it is
verified. Where something is untested or incomplete, it says so. Detailed
references: `ARCHITECTURE.md`, `TEST_RESULTS.md`, `COMPATIBILITY.md`,
`SECURITY.md`.

---

## 1. Executive summary

Distro Buny is a working UEFI multiboot system for x86-64 Linux ISO images,
implemented from scratch. Its components:

* a **graphical boot menu**: an unprivileged Linux program with its own
  software renderer, mouse/keyboard/touchpad input and the Distro Buny brand;
* a **boot engine**: it reads each ISO, interprets its boot configuration,
  plans a boot through one of twelve distribution adapters and starts the
  distribution with kexec;
* a **virtual medium**: a pre-init program that presents the ISO's bytes as a
  read-only disc;
* a **large-capacity drive format**: a deterministic GPT/exFAT/FAT32 writer;
* **host tools**: build, safe installation, verified updates, a test lab.

**Verified in QEMU/OVMF** with 45 real ISO images (169.7 GiB, 10 distribution
families):

* without Secure Boot, **44 of 45** reached the distribution's user space or
  installer; Debian's netinst installer is the known exception;
* with Secure Boot enforced, **all 15 Canonical-signed images** start. The 29
  others are refused by the kernel's signature check, as Secure Boot
  requires, and the menu predicts and explains this.

Installed drives written by the installer boot as well. A deliberately
fragmented ISO boots. All automated suites pass: 103 C unit checks, 46 Python
unit and integration tests and 3 virtual-machine boot tests.

**Not verified:** real computers and physical USB drives. The project's
safety rules forbade writing physical disks or restarting the host, so every
boot test ran in virtual machines.

## 2. Product vision

One USB drive that carries a whole library of Linux systems: as simple as
copying ISO files into folders, as polished as a modern operating system's
interface, and honest about what will and will not work on the computer in
front of it. Distro Buny aims to be a product, not a script collection:
original code, safe tools, measured compatibility, and a design that can
grow (more families, Secure Boot for more vendors, more platforms).

## 3. Completed feature overview

| Requirement | Status | Evidence |
| --- | --- | --- |
| Original implementation, no Ventoy/Easy2Boot/YUMI | **Done** | entire source tree; §8 |
| Bootable distribution image | **Done** | `output/images/distrobuny-1.0.0-full.img`, boots in OVMF |
| Modern graphical interface, branding | **Done** | screenshots in `documentation/screenshots/` |
| Mouse and keyboard navigation | **Done**, tested offscreen and in a VM | `test_ui_navigation.py`, `test_vm_menu.py` |
| Automatic ISO discovery, Desktop/Server categories, nested folders, Back | **Done** | same tests |
| Metadata, compatibility status, error handling | **Done** | badges, detail page, error dialogs |
| Real ISO boot integration | **Done**: 44/45 (UEFI), 15/45 (Secure Boot) | `COMPATIBILITY.md` |
| Extensible distribution adapters | **Done**, 12 adapters | §11 |
| Large-capacity storage design | **Done**: exFAT data partition filling the drive | §15 |
| ISO maintenance and update tool | **Done**: verified downloads; live metadata check against vendors | §17 |
| Reproducible build system | **Done**: bit-identical images | §16 |
| Safe USB installation tool | **Done**, tested on stand-in drives | §25, `TEST_RESULTS.md` §6 |
| Security and integrity safeguards | **Done** | §18, `SECURITY.md` |
| Automated tests, VM boot tests | **Done** | §19, §20 |
| Documentation and report | **Done** | `documentation/` |
| Physical hardware validation | **Not done** (outside the permitted environment) | §23 |
| Legacy BIOS, ARM64, Windows ISOs | **Not implemented** | §26 |

## 4. System architecture

Firmware → shim (Microsoft-signed) → GRUB (Canonical-signed) → Linux
runtime (Canonical-signed kernel + Distro Buny initramfs). Inside the runtime,
`buny-init` (PID 1, privileged) owns hardware, drives, validation and kexec.
`buny-ui` (unprivileged) draws the menu and parses everything that comes
from ISO files. After the user's choice, the distribution's own kernel and
initrd are started with `kexec_file_load`. A pre-init appended to the initrd
exposes the ISO as `/dev/loop200` before the distribution's init runs, so
live and installer initrds find their medium natively. `ARCHITECTURE.md` §1
has the diagram.

## 5. Important design decisions

| Decision | Alternatives considered | Why |
| --- | --- | --- |
| Menu runs in a Linux runtime, not in the bootloader | GRUB theme/scripts; a custom EFI application | Real graphics (DRM/KMS), USB HID input including touchpads, threads, file systems and full C libraries. A GRUB menu cannot provide the required UX. A custom EFI app would need its own drivers and a Microsoft signature. |
| Start distributions with kexec | chain-loading each ISO's GRUB/EFI loader via loopback | Firmware-independent and fast. One code path for all families. Lockdown enforces signatures under Secure Boot. Chain-loading needs per-ISO GRUB hacks and does not solve the medium problem. |
| Virtual medium (loop device from the file's extents) | per-family ISO-file parameters only (`iso-scan`, `img_loop`, `findiso`) | Many initrds have no ISO-file support (linuxrc, Guix, d-i), and existing ones break on paths with spaces. A block device with the ISO's label/UUID is what every initrd already understands. Native parameters remain available as an option. |
| Boot chain: Ubuntu's signed shim/GRUB/kernel | own shim (needs Microsoft review), unsigned chain | Works on Secure Boot PCs today without a signing programme. The cost is Secure Boot trust limited to Canonical/db/MOK keys (§18). |
| exFAT data partition (entry 1 of the GPT, last on the disk), FAT32 ESP | one FAT32 partition; ext4; NTFS | Cross-platform read/write, no 4 GiB limit, simple on-disk structure for the extent mapper. First in the table for OSes that mount only the first partition; last on the disk so the runtime can grow it to the end of a bigger stick (ARCHITECTURE.md §2). |
| Deterministic Python image writers | `mkfs.exfat` + `mtools` / loop mounts | No root needed, contiguous allocation, reproducible output, a manifest of every file's offset. Validated by the reference fsck tools. |
| Privilege separation (init vs UI) | one root process | Untrusted ISO parsing runs without privileges. |
| Lab automation via QEMU fw_cfg | special test images | The released image is tested unmodified. |

## 6. Source code organisation

```
source/common, filesystem, iso-manager, boot-adapters   C core shared by runtime and host tools
source/boot          buny-init, pre-init, hand-off, cpio, extents, IPC, module loader
source/security      firmware / Secure Boot / lockdown / trusted keys
source/interface     the menu (display, graphics, fonts, input, theme, icons, app, render)
source/tools         bunyctl
source/python/distrobuny   image writers, build pipeline, installer, updater, QEMU lab
configuration/       pinned boot packages, identification rules, compatibility records, update sources and keys
tools/               buny-build, buny-install, buny-update, buny-test-matrix, buny-compat, buny-icon-attribution
tests/               unit (C, Python), integration, boot, compatibility/runs (evidence)
```

Size: 17,600 lines of C in 35 files (excluding the vendored
`stb_truetype.h`), 4,100 lines of Python in the package, 770 lines of
command-line tools, 2,000 lines of tests, about 1,700 lines of
documentation. `DEVELOPER_GUIDE.md` §1 has the file-level map.

## 7. Programming languages and libraries

* **C11** (GCC, statically linked against glibc) for everything that runs at
  boot time and for `bunyctl`. Only libc and Linux UAPI headers are used,
  plus the single-header `stb_truetype` for font rasterisation. Graphics,
  PNG output, JSON, SHA-256, cpio, ISO 9660, exFAT, GPT, the GRUB
  interpreter and the input stack are original code.
* **Python 3** (standard library only) for host tools: image writers, build
  pipeline, installer, updater, test lab. GnuPG is invoked for OpenPGP
  verification.
* **GRUB script** for the small generated `grub.cfg`.

## 8. Original components and external dependencies

**Original (Distro Buny):** the menu and renderer; ISO 9660/Rock
Ridge/Joliet reader; GRUB script interpreter; systemd-boot, SYSLINUX,
treeinfo and identity parsers; identification rules engine; catalog and
index; 12 adapters and the planner; runtime init with module loader and
drive discovery; kexec hand-off; pre-init with loop and device-mapper
attachment; exFAT extent mapper; cpio writer; GPT, FAT32 and exFAT image
writers; installer; updater; QEMU lab and test suites.

**External, used unmodified:** shim 15.8, GRUB 2.12 (signed EFI binaries),
Linux 7.0.0-38-generic kernel and modules, BusyBox 1.36.1 (all pinned
Ubuntu packages); stb_truetype 1.26; Inter typeface; Simple Icons marks;
the exFAT up-case table (specification data). Build and test tools: GCC,
zstd, cpio, dpkg-deb, QEMU, OVMF, genisoimage, dosfstools, exfatprogs (as
reference `fsck.exfat`).

**Ventoy-free statement:** no code, binaries, plugins, configuration
formats or file layouts from Ventoy, Easy2Boot, YUMI or any other multiboot
product are used, wrapped or imitated.

## 9. Boot sequence, from firmware to distribution

1. **Firmware** loads `\EFI\BOOT\BOOTX64.EFI` (shim) from the ESP and
   verifies its Microsoft signature when Secure Boot is on.
2. **shim** verifies and starts `grubx64.efi` (Canonical).
3. **GRUB** reads `grub.cfg`: hidden 1 s menu, records the ESP's partition
   GUID, reads optional `options.cfg`, loads `/distrobuny/vmlinuz` (verified
   via shim) and `initramfs.img`, kernel parameters include `kho=off`.
4. **Kernel** starts `buny-init`. With Secure Boot on, lockdown (integrity)
   is active.
5. **buny-init** mounts kernel file systems, loads drivers by modalias,
   finds the ESP by GUID and the data partition on the same disk, mounts it
   read-only, writes `runtime.json` (firmware state, trusted keys, drive),
   and starts the menu as user 100.
6. **buny-ui** opens the display (DRM/KMS) and input devices, scans the
   folders, restores metadata from `/.distrobuny/index.json` and probes new
   files in the background. The menu is ready about 5.6 s after power-on
   (median, QEMU/KVM).
7. **The user selects an image.** The interface builds a boot plan,
   extracts kernel and initrd(s) into memfds and asks init to boot.
8. **buny-init** validates the path, maps the ISO's extents, verifies the
   mapping, appends the pre-init overlay, calls `kexec_file_load`
   (signature check under Secure Boot), then unmounts and executes the kexec.
9. **The distribution kernel** starts with its initrd. The pre-init forks
   a helper and `exec`s the original init. The helper waits for the data
   partition, attaches `/dev/loop200` (via device-mapper for fragmented
   files) and verifies the ISO label.
10. **The distribution's initrd** finds its medium on `/dev/loop200` and
    boots normally: live desktop, installer, or login.

## 10. ISO discovery and compatibility handling

* Discovery: every `*.iso` below the data partition root, hidden folders
  skipped, depth and node limits, inode loop guard. The folder hierarchy is
  the menu hierarchy.
* Identification: 72 rule sections (`distributions.conf`) matched against
  the ISO label, file name, identity files, treeinfo, application ID and boot
  menu titles; version and edition extraction; kernel version and signer
  from the kernel image.
* Compatibility levels, reported honestly: *recognised* → *method known* →
  *prepared* (kernel and initrd present, plan valid) → *verified* (the lab
  booted this exact image: same label and size). Badges: Verified, Ready,
  Needs Secure Boot off (predicted from the kernel signature and the
  machine's keys), Erases a disk, Known issue, Not supported yet, Damaged.
* Errors at any stage (unreadable ISO, missing kernel, fragmented file that
  cannot be mapped, kexec refusal) reach the user as plain sentences with a
  remedy and a retry.

## 11. Distribution-specific boot adapters

Twelve adapters: `casper`, `archiso`, `miso`, `dracut-live`, `anaconda`,
`kiwi`, `linuxrc`, `debian-live`, `nixos`, `guix`, `debian-installer`
(recognises the image and explains why it is blocked) and `generic-loopback`
(follows an ISO's own `loopback.cfg`). Each has a `detect()` score and
`prepare_virtual()`/`prepare_native()` functions. The planner ranks the
ISO's own boot entries (excluding memtest and network boot, deprioritising
media checks and rescue entries). `ARCHITECTURE.md` §5 lists the parameters
per family; `DEVELOPER_GUIDE.md` §4 shows how to add one.

Measured: all ten booting families pass (`COMPATIBILITY.md`). `debian-live`
and `generic-loopback` are implemented but no ISO in the collection
exercises them: **not tested**.

## 12. Graphical interface architecture

A single-threaded render loop with worker threads for probing and booting,
woken by an eventfd. A screen stack holds home, folder, detail, boot
progress, settings, system information, search, log and no-drive screens,
plus dialogs. Layout and drawing are separated per screen. Items carry
stable keys for focus and animation. Rendering is done in software into a
premultiplied BGRA buffer, using signed-distance rounded rectangles, cached
shadows, an exact-coverage path rasterizer, stb_truetype text with sub-pixel
positioning and gamma correction, and dithered gradients. The result is
flushed through DRM dumb buffers (fbdev fallback). Light and dark themes are
built from colour tokens, and everything scales automatically from 800×600
to 4K. Lab mode reports screens, focus, items and badges as events for
tests.

## 13. Branding and icon integration

The official logo (`Distro buny logo.png`) is converted at build time into a
transparent vector-quality mark by un-blending the white paper (preserving
anti-aliasing). It is shown on the start page with the product name and
tagline. Distribution marks are SVG paths from Simple Icons, drawn as
vectors in each brand's colour; families without a mark get monogram badges.
Licences and brand guidelines for each mark are listed in
`assets/distro-icons/ATTRIBUTION.md` (§24). The palette and Inter typeface
are used consistently in both themes.

## 14. Mouse and keyboard support

evdev devices are discovered at start and on hot-plug (inotify):
keyboards, relative mice, absolute pointers (tablets, VMs, touchscreens)
and touchpads (tap-to-click, two-finger scrolling). Keyboard: arrows with
spatial focus, Tab/Shift+Tab, Enter/Space, Esc/Backspace (back), Home,
Page Up/Down, F10 (power), type-to-search. Mouse: hover, click, right-click
back, wheel scrolling, breadcrumbs. Verified offscreen through the input
handlers (12 tests) and in a VM with QEMU's USB keyboard and USB tablet
(`test_vm_menu.py`). Touchpad gestures are **not tested** on real hardware.

## 15. Disk-image design and filesystem choices

GPT; partition 1 `DISTROBUNY` exFAT filling the drive (cluster size scales
with the volume, 128 KiB for the reference image); partition 2 `BUNY-EFI`
FAT32, 512 MiB; 1 MiB alignment. Files are allocated contiguously
(`NoFatChain`), so an ISO maps to one extent. The runtime also handles
FAT-chained (fragmented) files: verified by booting an ISO split into
three out-of-order fragments. A manifest records every file's offset. The
reference image: 178.3 GiB for 45 ISOs (169.7 GiB). Sparse image files cost
only the data written. `ARCHITECTURE.md` §2.

## 16. Build system and reproducibility

`make` builds the C programs (host and static runtime). `buny-build`
drives the rest: pinned-package download with SHA-256 verification
(`components.json`), runtime initramfs assembly (module selection, BusyBox,
assets), ESP assembly, image writing, manifest and checksum, and fast ESP
refresh. With `SOURCE_DATE_EPOCH`, identical inputs yield a bit-identical
image (`test_reproducible`). Partition GUIDs and file-system serials derive
from the build ID. The released image was built that way:
`SOURCE_DATE_EPOCH=1792713600`; the SHA-256 is in
`output/checksums/distrobuny-1.0.0-full.img.sha256`.

## 17. ISO update subsystem

22 trusted sources (Ubuntu and nine flavours via a template, Debian, Arch,
Mint, five Fedora editions, openSUSE Tumbleweed and MicroOS) with 7 pinned
vendor keys. `check` verifies the vendor's signed checksum list before
comparing versions. `update` downloads resumably into the drive's staging
area, verifies the SHA-256 and replaces atomically after approval. Live run
against the real vendor servers on 23 October 2026: 24 images checked in
17.5 s, 3 updates found, all metadata signature-verified, nothing written.
Downloading real multi-GiB updates was **not exercised** (covered by local
tests). `ARCHITECTURE.md` §8, `SECURITY.md` §6.

## 18. Secure Boot and security considerations

* The signed chain (Microsoft → Canonical) starts on Secure Boot PCs. Under
  lockdown the kernel accepts only kernels signed by keys it trusts:
  Canonical's, the firmware `db`, MOK.
* Measured: 15/15 Canonical-signed images start. Fedora/openSUSE/Bazzite
  kernels (signed by their own CAs) and unsigned kernels are refused. The
  menu predicts this per machine from `/proc/keys`, marks such images, and
  offers a restart into firmware settings.
* No bypass of any kind. The initramfs is unsigned, as in standard Ubuntu.
  Distro Buny has no signing key of its own. `SECURITY.md` §2.4 states the
  consequences.
* Privilege separation; bounded parsers; validated IPC; read-only data and
  virtual medium; verified medium mapping; destructive-image warnings;
  installer refusing system disks; signed-update verification.

## 19. Automated test results

| Suite | Result |
| --- | --- |
| C unit tests | 103 checks, 0 failures |
| Python unit tests (image 6, installer 17, updater 11) | 34/34 PASS |
| Interface integration (offscreen, real program) | 12/12 PASS |
| Menu in a VM (UEFI; UEFI + Secure Boot badge prediction) | 2/2 PASS |
| Fragmented ISO boot | PASS |

Details: `TEST_RESULTS.md` §2–§4.

## 20. Virtual-machine boot test results

| Run | Result |
| --- | --- |
| 45 images, UEFI | 44 PASS, 1 BLOCKED (after fixing 1 runtime and 2 harness defects found by the first run) |
| 45 images, UEFI + Secure Boot | 15 PASS, 29 refused by the signature policy, 1 BLOCKED |
| Forced device-mapper medium (4 images) | 4 PASS |
| Fragmented ISO (3 extents) | PASS |
| Drives written by `buny-install create` / `write` | 6 PASS, 1 BLOCKED (Debian) |
| Released image | 4/4 PASS (UEFI); Ubuntu PASS and Fedora refused as predicted (Secure Boot); menu VM tests PASS in both modes |

Each run directory in `tests/compatibility/runs/` contains serial logs,
screenshots, QEMU command lines and per-image results.

## 21. Verified distribution compatibility

Ubuntu 26.04.1/26.10 and Edubuntu, Kubuntu, Lubuntu, Ubuntu Budgie, MATE,
Unity, Cinnamon, Kylin, Studio, Xubuntu; Ubuntu Server; Linux Mint 22.3
(Cinnamon, Xfce); Zorin OS 18.1; Pop!\_OS 24.04 (2); Fedora 44 Workstation,
KDE, Silverblue, Kinoite, Budgie/Sway/COSMIC Atomic, IoT, Server; Bazzite
(2); Nobara 44; Arch Linux; EndeavourOS; CachyOS; Omarchy; Manjaro 26.1 (2);
openSUSE Leap 16.0, Tumbleweed, MicroOS, Leap Micro; NixOS 26.05; Void;
Gentoo; GNU Guix 1.5.0. **Blocked:** Debian 13.7 netinst. Per-image table
with Secure Boot results: `COMPATIBILITY.md`.

## 22. Performance measurements

QEMU/KVM on the development host (45-image run): menu ready at 5.6 s
median after power-on (5.4–17.8 s); kernel loaded 7.9 s; hand-off 8.6 s;
virtual medium attached 14.0 s; distribution user space 41.7 s median
(21–138 s, dominated by each distribution's own boot). Hand-off overhead
(load → kexec) 0.7 s median. The menu is ready about 0.15 s after the
runtime starts it. Build: 45-ISO image written in 548.5 s (~690 MiB/s);
installer writes and verifies a 1.6 GiB stand-in drive in 7 s.
`TEST_RESULTS.md` §8.

## 23. Outstanding defects and technical constraints

| Item | Impact | Plan |
| --- | --- | --- |
| No physical hardware testing | firmware quirks, USB controller timing and GPU-specific issues unknown | hardware lab (`FUTURE_DEVELOPMENT.md` §4) |
| Debian netinst/DVD blocked | Debian installer images cannot start (Debian live is supported) | inject the ISO's loop/iso-scan udebs (§2 there) |
| Secure Boot limited to Canonical/db/MOK-trusted kernels | Fedora, openSUSE, Bazzite need Secure Boot off or MOK enrolment | guided MOK enrolment; own shim (§1 there) |
| Self-installing images may offer the Distro Buny drive as target | user must choose carefully (warning + confirmation implemented) | pass device-exclusion options (§3 there) |
| kexec memory: large initrds need RAM headroom | very-low-RAM machines may fail with a clear message | stream initrds (§4 there) |
| Untrusted-input parsers not fuzzed | robustness unknown beyond tests | libFuzzer (§8 there) |
| "Verified" badge matched by label and size, not hash | a modified file of identical size would still show the badge | in-menu hash verification (§6 there) |
| Legacy BIOS, ARM64, Windows ISOs | unsupported | §4 there |

No known failing test remains. Defects found during testing, all fixed and
re-verified:

* kexec handover (on by default in the runtime kernel) made `kexec_file_load`
  fail for the 349 MiB openSUSE DVD initrd (`kho=off`);
* lab console parameters were appended after Mint's `--` and went to init;
* the device-mapper path for fragmented files did not provide `/dev/loop200`,
  which three adapters name directly;
* signed-but-untrusted kernels were shown as *Ready* under Secure Boot
  (now predicted from the machine's trusted keys);
* folder rules replaced user folder names on partial matches;
* the published checksum went stale after an ESP refresh;
* the runtime's BusyBox came from the build host (now a pinned package);
* the icon licence notice claimed CC0 for every mark;
* three distributions' success was not recognised by the test harness.

## 24. Dependencies and applicable licence obligations

| Component | Licence | Obligations when distributing an image |
| --- | --- | --- |
| Distro Buny source | GPL-3.0-or-later | provide the source |
| shim 15.8 (`shim-signed`) | BSD-2-Clause | keep the notice |
| GRUB 2.12 (`grub2`) | GPL-3.0-or-later | provide corresponding source (Ubuntu source package of the same version) or a written offer |
| Linux 7.0.0-38 (`linux-hwe-7.0`) | GPL-2.0-only | same |
| BusyBox 1.36.1 | GPL-2.0-only | same |
| GNU C Library 2.39 (`glibc`, linked statically into buny-init, buny-ui, buny-preinit) | LGPL-2.1-or-later | provide the library's source, and let users relink: the Distro Buny source is published |
| stb_truetype 1.26 | public domain / MIT | none (notice kept) |
| Inter | SIL OFL 1.1 | keep the licence with the fonts; do not sell the fonts alone |
| Simple Icons marks | CC0 collection. Per-mark: Debian CC-BY-SA-3.0, Gentoo CC-BY-SA-2.5, NixOS CC-BY-4.0, Rocky CC-BY-SA-4.0, Garuda and MX GPL-3.0-only, Omarchy MIT, Fedora brand terms | attribution (`ICON-ATTRIBUTION.md`); follow brand guidelines; trademarks remain the owners' |
| exFAT up-case table | specification data; bytes as in exfatprogs (GPL-2.0-or-later) | compatible with GPL-3.0-or-later |
| ISO images in the collection | their publishers' licences | redistribution of a filled image must respect each distribution's terms and trademarks |

Every image carries `distrobuny/LICENSES.txt`, `ICON-ATTRIBUTION.md` and the
full licence texts (`distrobuny/licenses/`) on its ESP. Before publishing
images, the publisher must make the corresponding source of the GPL and LGPL
components available (the pinned Ubuntu source packages). Only the starter
image, which has no ISO files, is published; `buny-build release` builds the
download folder with the source notice `SOURCE-CODE.txt`, a script that
fetches the exact source packages, and the Distro Buny source archive
(`BUILD_INSTRUCTIONS.md` §9).

## 25. Build and installation instructions

```bash
tools/build/buny-build doctor
make -j"$(nproc)"
tools/build/buny-build runtime && tools/build/buny-build esp
SOURCE_DATE_EPOCH=1792713600 tools/build/buny-build image --profile full
tools/maintenance/buny-install list
sudo tools/maintenance/buny-install create --isos "Iso layout" --target sdX
```

`BUILD_INSTRUCTIONS.md` covers requirements, profiles, tests and VM
testing; `USER_GUIDE.md` covers installation and use.

## 26. Future engineering recommendations

In order of value: (1) Secure Boot for more vendors through guided MOK
enrolment, and later Distro Buny's own signed shim; (2) Debian installer support;
(3) origin-aware self-installers; (4) a real-hardware test lab and firmware
quirk handling; (5) streaming initrds for low-memory machines; (6) more
signed update sources and graphical installer/updater front ends; (7)
persistence, localisation and accessibility; (8) fuzzing and CI.
`FUTURE_DEVELOPMENT.md` gives the approach for each.
