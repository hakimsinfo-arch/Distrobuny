# Distro Buny — Test Results

Status of every test suite for Distro Buny 1.0.1, as run on 24 October 2026
and again after the fixes of 28 October 2026 (Debian installer)
(the distribution matrices and every suite below were re-run on the final
1.0.1 build; sections marked 1.0.0 describe earlier runs kept for history)
on the development host (Ubuntu 24.04, Linux 7.0, 12 CPUs, KVM, QEMU 8,
OVMF). Results are **PASS**, **FAIL**, **BLOCKED** (no working method
exists, by design or not yet) or **NOT TESTED**. Nothing here is estimated:
each line points to a test file, a log or a run directory.

## 1. Summary

| Area | Tests | Result |
| --- | --- | --- |
| C unit tests (`make test`) | 118 checks in 11 groups | **PASS** (0 failures; 28 October 2026) |
| Image writers and drive expansion (`tests/unit/python/test_image.py`) | 14 | **PASS** |
| Release folder checks (`tests/unit/python/test_release.py`) | 9 | **PASS** |
| Installer (`tests/unit/python/test_install.py`) | 17 | **PASS** |
| Updater (`tests/unit/python/test_updater.py`) | 11 | **PASS** |
| Interface integration (`tests/integration/test_ui_navigation.py`) | 25 | **PASS** |
| Parser robustness (`tests/robustness/fuzz_probe.py`, ASan + UBSan) | 3,000 corrupted images | **PASS** (0 crashes, hangs or sanitizer reports; 765 refused cleanly) |
| Menu in a VM (`tests/boot/test_vm_menu.py`) | 2 scenarios (UEFI; UEFI + Secure Boot) | **PASS** |
| Starter image in a VM (`tests/boot/test_starter_image.py`) | 5 (6 boots of a grown drive) | **PASS** |
| Fragmented ISO boot (`tests/boot/test_fragmented_boot.py`) | 1 | **PASS** |
| Distribution boots, UEFI (54 images, run `final-1.0.1`) | 54 | **53 PASS, 1 BLOCKED** (Debian netinst, fixed on 28 October, rows below) |
| Distribution boots, UEFI + Secure Boot (54 images, run `final-1.0.1-sb`) | 54 | **15 PASS**, 38 refused by Secure Boot as designed (20 unsigned, 18 untrusted CA), 1 BLOCKED |
| Cross-family re-run of the final build (21 images, run `final-1.0.1-regress`) | 21 | **21 PASS** (after the pre-init now repeats its udev announcement) |
| Debian installer after the fix (runs `fix-1028-debian-1..3`) | 3 | **3 PASS** (network detection reached through the virtual medium); the graphical installer without lab console reached the hostname question |
| Re-check of the fixed runtime, UEFI (24 images, run `fix-1028-regress`) | 24 | **24 PASS** (every family still available; `COMPATIBILITY.md`) |
| Re-check, UEFI + Secure Boot (sample, run `fix-1028-regress-sb`) | 6 | **3 PASS** (Ubuntu Desktop, Server, Zorin), 3 refused as designed (Debian, Fedora, Arch) |
| Build and every suite inside an Ubuntu 26.04.1 LTS VM (27 October, before the Debian fix) | 23 steps | **PASS** (after two fixes it found: Pillow check, tmpfs test helper) |
| Installed drives boot (`buny-install create` / `write`) | 7 | **6 PASS, 1 BLOCKED** (1.0.0 layout; not re-run for 1.0.1) |
| Live update check against vendor servers | 45 images | **PASS** (24 checked with verified signatures, 3 updates found; 1.0.0, not re-run) |
| Real hardware, physical USB drives | — | **NOT TESTED** (see §9) |

## 2. Unit tests

### C (`tests/unit/c/`)

```
$ make test
core: 35 checks, 0 failures
isoboot: 68 checks, 0 failures
```

| Group | What is checked |
| --- | --- |
| `sha256_vectors` | FIPS 180-4 vectors, one million 'a', block-boundary updates |
| `json_roundtrip` | parser/writer round trip, malformed inputs rejected, 5000-deep nesting refused |
| `strings` | natural sort, sizes, path helpers |
| `cmdline_quoting` | quoting of arguments with spaces; kernel parameters kept in front of `---` and `--` |
| `kernel_keys` | parsing `/proc/keys` into trusted key subjects (duplicates and non-X.509 entries dropped) |
| `iso_reader` | genisoimage fixtures: Rock Ridge, Joliet, symlinks, case-insensitive lookup, globbing, kernel header |
| `grub_interpreter` | variables, `if/elif`, `for` with globbing, functions, `submenu`, `regexp`, `search`, `loopback.cfg` with `iso_path` |
| `other_parsers` | systemd-boot entries, SYSLINUX with `::/` paths, `.treeinfo`, `.disk/info` |
| `planning` | adapter choice and generated parameters for casper, archiso and others; names with spaces; blockers |
| `debian_installer` | the plan carries the image's `loop-modules` udeb; lab: text installer, serial console only, language answered; blocked without the udeb |
| `folder_names` | folder rules rename only on whole-name matches ("open suse" → "openSUSE", "Ubuntu Family" kept) |

### Python (`tests/unit/python/`)

| Suite | Tests | What is checked |
| --- | --- | --- |
| `test_image.py` | 14 | GPT round trip with CRCs and 1 MiB alignment; ESP first on the disk, DISTROBUNY entry 1 and last; ESP passes `fsck.fat`; data partition passes the reference `fsck.exfat`; the C exFAT reader finds every file at the writer's offsets with identical content; spec up-case table and name hashes; **two builds are bit-identical**. `ExpandTest` (8): growing DISTROBUNY on a bigger "stick" with `bunyctl expand` (the runtime's code): both GPT copies, `fsck.exfat -n`, `fsck.fat -n`, file bytes and an unchanged ESP; a **power cut after every single write** (1-11 of 12) then finishing gives a byte-identical result; a torn main boot region; a primary GPT rebuilt by firmware; 128 KiB clusters on a 200 GiB stick (bitmap grows by a cluster); a plain volume grows only as far as its bitmap; refusals (1.0.0 layout, dirty volume, reformatted partition, devices) write nothing |
| `test_install.py` | 17 | drive rules on synthetic sysfs/procfs: system disk refused even with override, mounted stick refused, LUKS `/home` on a USB disk counts as system, LVM member refused, swap, card reader without medium, 4Kn and write-protected disks, too small, partition names rejected; writer: image onto a larger, dirty "used" drive (old ISO signature cleared, backup GPT at the real end, fresh GUIDs, contents identical, both file systems pass fsck), verification catches silently dropped writes, refusals, create mode filling the drive, segment painter vs a reference model |
| `test_updater.py` | 11 | local HTTP server with Range support and throw-away OpenPGP keys: newer release found and verified; impostor signature rejected; corrupted download discarded with the original kept; interrupted download resumes; atomic replacement and `--keep-old`; non-volume folders and plain HTTP refused; checksum formats; version ordering and LTS series |

## 3. Interface integration tests

`tests/integration/test_ui_navigation.py` builds a small ISO collection
(Ubuntu-style and Arch-style images, a KIWI self-installer, an image around
the real Canonical-signed runtime kernel, nested folders, names with spaces,
a hidden folder, a damaged file). It runs the real interface program
offscreen, driven by keyboard and pointer scripts through the normal input
handlers. A second set runs the program on an empty drive (only `Desktop`,
an empty sub-folder, and `Servers`), a third one with the runtime's offer to
grow the drive, a fourth with an unknown family and an unattended installer.
25 tests, all **PASS**:

| Test | Checks |
| --- | --- |
| `test_discovery` | 7 images found; the hidden `.distrobuny` folder ignored |
| `test_keyboard_hierarchy_and_back` | Enter descends three levels; Esc and Backspace return level by level |
| `test_keyboard_moves_focus_spatially` | arrow keys move focus between cards |
| `test_mouse_hierarchy_breadcrumbs_and_back` | clicks on cards, a breadcrumb, the Back button |
| `test_boot_preparation_through_the_menu` | detail page → Start → casper plan with `live-media=/dev/loop200` before `---` (dry run) |
| `test_names_with_spaces_boot_through_the_virtual_medium` | `/Servers/my server image.iso` → archiso plan |
| `test_damaged_image_cannot_start` | marked *Damaged*; no boot is prepared |
| `test_self_installing_image_needs_confirmation` | warning dialog; the default (Enter) cancels; *Start anyway* proceeds |
| `test_secure_boot_trust_prediction` | Secure Boot on: Canonical-signed kernel *Ready* when Canonical's CA is trusted, *Needs Secure Boot off* when it is not; unsigned → *Needs Secure Boot off*; Secure Boot off → *Ready* |
| `test_settings_dark_theme` | settings page, theme switched to dark |
| `test_search` | typing filters; the result opens |
| `test_small_and_large_displays` | 800×600 and 3840×2160 layouts |
| `EmptyDriveTest.test_home_explains_how_to_add_images` | no images: Home shows the "No ISO images on this drive yet" steps; the folders can still be opened |
| `EmptyDriveTest.test_empty_folders_explain_how_to_add_images` | an empty folder and an empty sub-folder show "No ISO images in this folder yet" with their path |
| `EmptyDriveTest.test_small_display` | the empty-drive steps at 800×600; no expansion offer without the runtime's |
| `UnknownAndUnattendedTest.test_unknown_family_starts_best_effort_after_a_confirmation` | an Alpine-style image (no adapter knows it): badge *Best effort*, Start asks first (Enter cancels), then the image's own entry is prepared (`adapter=best-effort`, its arguments kept) |
| `UnknownAndUnattendedTest.test_unattended_installer_is_marked` | an Ubuntu image with `autoinstall`: badge *Erases a disk*, Start needs *Start anyway* |
| `ExpandOfferTest.test_home_offers_to_use_the_whole_drive` | runtime.json `expand.state=possible`: Home shows the *Use the whole drive* banner beside the empty-drive steps |
| `ExpandOfferTest.test_expansion_needs_a_deliberate_confirmation` | Enter on the dialog's initial focus (Cancel) closes it; only Down + Enter on *Expand DISTROBUNY* sends the request |
| `ExpandOfferTest.test_mouse_confirmation` | the same with the mouse: Cancel, then *Expand DISTROBUNY* |
| `ExpandOfferTest.test_refused_drive_explains_why` | `state=refused`: the banner opens the runtime's reason; no request |
| `ExpandOfferTest.test_small_display` | the banner fits 800×600 and overlaps nothing |

Screenshots: `build/test-artifacts/ui/`.

## 4. Virtual-machine boot tests

### 4.1 The menu in a VM (`tests/boot/test_vm_menu.py`), **PASS**

On the released full image, through OVMF → shim → GRUB → runtime, with
QEMU's virtual USB keyboard and USB tablet:

* interface ready on the DRM/KMS display with keyboards and pointers detected;
* keyboard: Enter opens *Desktop*, Esc returns;
* tablet: click on *Server*; right-click returns; click through
  *Desktop* → *Arch Linux*; the *Desktop* breadcrumb returns;
* detail page → **Start**: the privileged runtime prepared the complete
  hand-off for the Arch ISO (extent map, medium verification, pre-init
  overlay, kernel command line). `buny.dryrun=1` stops just before
  `kexec_file_load`;
* Esc three times back to the start page, F10 opens the power dialog.

Second scenario, **with Secure Boot enforced**: the runtime reported 19
trusted kernel keys and restored all 45 catalog entries from the drive's
index. Opening the *Fedora*, *Ubuntu* and *Arch Linux* folders through the
menu, every Fedora card (kernel signed by Fedora's CA) and every Arch-family
card (unsigned) showed **Needs Secure Boot off**, every Ubuntu card showed
**Verified** (from the Secure Boot lab records), and Debian netinst showed
**Not supported**. These predictions match the measured Secure Boot results
for all 27 images.

Screenshots: `build/test-artifacts/vm-menu/` (copies in `documentation/screenshots/`).

### 4.2 Distribution boot matrix

`tools/development/buny-test-matrix` boots each image by selecting it in
the menu (lab auto-select) and watches the serial console. A run passes
only when the distribution's own user space is reached through the virtual
medium (`COMPATIBILITY.md` defines the markers).

| Run | Image | Firmware | Result |
| --- | --- | --- | --- |
| `full-1` | full image (first build) | UEFI | 37 PASS, 7 FAIL, 1 BLOCKED |
| `full-2-fixes` | full image, fixed runtime | UEFI | 7/7 PASS (the former failures) |
| `full-sb` | full image | UEFI + Secure Boot | 14 PASS, 29 refused (by design), 1 FAIL (harness), 1 BLOCKED |
| `full-sb-2` | full image | UEFI + Secure Boot | 1/1 PASS (Ubuntu Server re-run after the harness fix) |
| `dm-path`, `dm-path-full` | dev / full image, `buny.medium=dm` | UEFI | 4/4 PASS |
| `fragmented` | dev image, Void ISO in 3 fragments | UEFI | 1/1 PASS |
| `install-e2e-create` | drive written by `buny-install create` | UEFI | 1/1 PASS |
| `install-e2e-write` | drive written by `buny-install write` | UEFI | 5 PASS, 1 BLOCKED |
| `final-smoke`, `final-smoke-sb` | full image, release candidate | UEFI / Secure Boot | 10/10 PASS; Mint and Ubuntu PASS, Fedora refused |
| `release-smoke`, `release-smoke-sb` | **released** full image | UEFI / Secure Boot | see §4.3 |

How the 7 failures of `full-1` were analysed and fixed (one real runtime
defect, kexec handover with large initrds, and two harness defects) is
described in `COMPATIBILITY.md`, "History of the results". Every run
directory holds per-image serial logs, screenshots, QEMU command lines and
`result.json`.

### 4.3 Final smoke test on the released image

The released image (`distrobuny-1.0.0-full.img`, SHA-256 in
`output/checksums/`) was re-checked after its last ESP refresh:

| Check | Result |
| --- | --- |
| `release-smoke`: Arch (archiso), Fedora Workstation (dracut-live), openSUSE Tumbleweed DVD (linuxrc, 349 MiB initrd), Ubuntu 26.04.1 (casper) | **4/4 PASS** |
| `release-smoke-sb`: Ubuntu 26.04.1 with Secure Boot | **PASS** |
| `release-smoke-sb`: Fedora Workstation with Secure Boot | refused by the kernel's signature check (`ENOKEY`), as expected and as predicted by the menu |
| Menu in a VM, both scenarios (§4.1) | **PASS** |
| Catalog index restored at boot | 45 of 45 entries (`BUNY-EVENT catalog images=45 restored=45`) |

The release-candidate run before the last refresh (`final-smoke`) booted one
image of every other family as well (Manjaro, Fedora Silverblue, Leap 16,
NixOS, Guix, Gentoo): 10/10 PASS. The last refresh changed only test hooks,
licence notices and log events, not boot logic.

### 4.4 The starter image (the public download)

`distrobuny-1.0.0-starter.img` (3.5 GiB, no ISO files; SHA-256 in
`output/checksums/`) was built with `SOURCE_DATE_EPOCH=1792713600`. It uses
the growable layout (ESP first, DISTROBUNY last with 128 KiB clusters and
room for a 2 TiB FAT, ARCHITECTURE.md §2). Its boot partition is newer than
the full and dev images': it has the menu's empty-drive steps, the drive
expansion and the full licence texts (`distrobuny/licenses/`). The full and
dev images were not rebuilt: they keep the 1.0.0 layout and the earlier
boot partition, which §4.1–4.3 describe.

| Check | Result |
| --- | --- |
| Two builds bit-identical | **PASS** (same SHA-256) |
| ESP vs `fsck.fat -n`, data partition vs the reference `fsck.exfat -n` | **PASS**, both clean (`directories 4, files 3`) |
| `buny-vm --display none --lab`: runtime finds the DISTROBUNY partition, menu ready | **PASS** (`ui-ready isos=0`, `ui hint="No ISO images on this drive yet"`) |
| `test_empty_drive_on_a_bigger_stick`: the image at the start of a 16 GiB USB stick (as Etcher, Rufus and `dd` leave it; backup GPT mid-disk) | **PASS**: OVMF starts it, the data partition is found by UUID, Home and an empty folder show the steps |
| `test_iso_copied_onto_the_drive_appears`: starter layout plus a Void Linux ISO in `Desktop`, with the drive's index still the empty one | **PASS**: 1 image listed, the hand-off prepared (dry run) |
| `test_expanding_the_drive_from_the_menu`: Home offers *Use the whole drive* (`expand-plan state=possible`; the kernel warned about the mid-disk backup GPT); Enter on the dialog's initial focus would cancel, *Expand DISTROBUNY* grows 3.2 GB to 16.6 GB in 9 writes / 49 ms; the kernel uses the new size at once; after *Restart* no GPT warning, `expand-plan state=none`, no offer | **PASS**; VM disk afterwards: both GPT copies valid, backup at the end, `fsck.exfat -n` and `fsck.fat -n` clean |
| `test_power_cut_during_the_expansion`: `buny.expandcrash=3` (inside the GPT step) and `=7` (inside the exFAT step, volume dirty) reset the machine mid-expansion | **PASS**: the next start finishes it before the menu (`expand-plan state=resume`, `expand-done`), no offer; disks fsck-clean. After the cut at 3, OVMF had already rebuilt the torn primary GPT from the new backup |
| `test_older_layout_is_refused`: the 1.0.0 layout (ESP behind DISTROBUNY) with the current runtime on a 16 GiB stick | **PASS**: `expand-plan state=refused code=layout`, the banner explains why, nothing written |
| Void Live booted (kexec, virtual medium) from a growable image expanded from 1.9 to 8 GiB with `bunyctl expand` | **PASS** (user space after 30 s; run in `build/vm/newlayout-void/`, not kept) |
| `buny-build release`: `.img.xz` decompresses to the image's SHA-256 | **PASS** |
| `buny-build release` with the full image | refused, as designed |

The ISO in the second scenario is written by Distro Buny's exFAT writer, not
by Windows or macOS. Copies by other systems, which can be fragmented, are
covered by the fragmented-boot test (`fragmented` run). Screenshots:
`build/test-artifacts/starter/`.

## 5. Image build and reproducibility

| Check | Result |
| --- | --- |
| Full image build (45 ISOs, 169.7 GiB of data, 178.3 GiB image) | 548.5 s to write, 12 min 48 s including the SHA-256 |
| Same inputs → bit-identical image | **PASS** (`test_reproducible`) |
| ESP refresh with checksum and manifest update | **PASS**, published SHA-256 always matches the image |
| Image file systems vs reference tools | `fsck.fat` and `fsck.exfat` clean (unit tests; the fragmented test volume too) |

## 6. Installer

| Check | Result |
| --- | --- |
| `buny-install list` on the development host | the only disk (NVMe, `/` and `/boot/efi`) shown as **refused**: running system, fixed disk |
| `write --target /dev/nvme0n1 --confirm nvme0n1` | **refused before opening the device** |
| `write --target /dev/nvme0n1p2` | refused: partition, whole drive required |
| `create` onto a 16 GiB stand-in drive (Arch) | 1.6 GiB written and read back in 7 s; boots (§4.2) |
| `write` dev image onto a 32 GiB stand-in drive | 6.9 GiB written and verified; backup GPT at the end; boots (§4.2) |
| Overwrite protection for stand-in files | refused without `--overwrite-file` |

## 7. Updater

| Check | Result |
| --- | --- |
| Unit tests (local server, throw-away keys) | 11/11 PASS (§2) |
| Live check against the vendors' servers (`buny-update --root "Iso layout" check`) | **PASS** in 17.5 s: 24 images with trusted sources checked; 21 current; 3 updates found (openSUSE Tumbleweed and MicroOS snapshot 20261021, Ubuntu Unity 24.04.4 → 26.04), each from checksum lists signed by the pinned vendor key; 21 images have no trusted source configured; nothing written to the collection |
| Downloading and applying a real update | **NOT TESTED** against vendor servers (multi-GiB downloads need approval); the full path is covered by the unit tests |

## 8. Performance (QEMU/KVM, from the `full-1` run, 45 images)

| Milestone (seconds after power-on) | min | median | max |
| --- | --- | --- | --- |
| Menu ready (firmware + shim + GRUB + runtime + catalog) | 5.4 | 5.6 | 17.8 |
| System kernel loaded (after auto-select) | 6.9 | 7.9 | 19.6 |
| Hand-off (kexec) | 7.6 | 8.6 | 20.3 |
| Virtual medium attached | 10.6 | 14.0 | 23.3 |
| Distribution user space | 21.4 | 41.7 | 138.0 |

* From "kernel loaded" to the hand-off: median 0.7 s. From the hand-off to
  the medium being attached: median 4.0 s, mostly the distribution's own
  early boot until its initrd runs the pre-init.
* With the build-time index, the menu is ready with full metadata for all
  45 ISOs about 0.15 s after the runtime starts it (`catalog images=45
  restored=45`, `ui-ready … ms=152` on the released image).
* The runtime initramfs is 20.7 MiB (zstd); the ESP holds 41.2 MiB.
* Image build: about 690 MiB/s while copying ISOs on the host NVMe;
  `create` installs onto a file stand-in at about 630 MiB/s write and
  510 MiB/s verify.

## 9. Not tested

| Item | Why |
| --- | --- |
| Real computers and physical USB drives | The project's safety rules forbid writing physical disks or rebooting the host; all boots ran in QEMU with OVMF. The installer's device path was tested on synthetic sysfs trees and on the refusal path with the real system disk. |
| Enrolling a vendor CA with the MOK manager | requires interactive firmware steps; the MOK manager is shipped |
| Debian live images | none in the test collection |
| Legacy BIOS | not supported |
| Touchpad gestures on real hardware | the VM provides a keyboard, mouse and tablet; touchpad code is untested on hardware |
| Downloading multi-GiB updates from vendor servers | avoided deliberately; covered by local tests |
| A grown drive in Windows or macOS | No Windows/macOS machine in the lab. The grown volume is standard exFAT (checked with exfatprogs' `fsck.exfat` and the Linux kernel driver); a FAT reserve in front of the heap and a bitmap at the end are allowed by the specification, but Microsoft's and Apple's drivers have not read one yet |
| Expanding a real stick | VM disks only; real USB sticks can reorder or drop writes despite cache flushes |
| Drives from `buny-install create`/`write` with the new layout booting a distribution | the installer's unit tests pass with the new layout; its end-to-end boots (§4.2 `install-e2e-*`) predate it |
