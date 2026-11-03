# Distro Buny changelog

## 1.0.1 (October 2026)

The first release meant for publication. 1.0.0 was built and shown for
review, but never published.

### Fixes of 28 October 2026 (same version)
* **Debian's installer images (netinst, DVD) start.** Their initrd has no
  loop driver and only accepts a CD drive or disk, so 1.0.1 blocked them.
  The image's own pool carries the loop driver for its installer kernel
  (`loop-modules-<release>-di` udeb): the plan hands it to the pre-init, which
  extracts the driver with the installer's BusyBox and loads it, and a udev
  rule tags the virtual medium as a CD drive. The installer then finds,
  mounts and loads its components from the virtual medium like a real CD.
  Tested with Debian 13.7.0 netinst (graphical and text installer).
* Lab: the Debian installer gets only the serial console (with two consoles
  it starts two installers, which corrupt each other's package database) and
  the text installer, with language and keyboard answered.
* Reproducible builds no longer depend on the folder the source tree is in:
  the build directory is mapped out of the debug information
  (`-ffile-prefix-map`), so the runtime binaries and their build IDs are
  identical wherever the tree is built. 1.0.1 images rebuilt in another
  folder differed from the published image only in these build IDs.
* `buny-build doctor` also checks Pillow (needed by the runtime build),
  `qemu-img`, `fsck.fat`, `genisoimage`, `xz` and LibreOffice, and prints the
  `apt install` line for whatever is missing on Ubuntu. A fresh Ubuntu 26.04
  passed the old check and then failed in `buny-build runtime`.
* The image tests copied partitions out of sparse images with a buffered
  reader whose position `SEEK_DATA`/`SEEK_HOLE` moved behind its back; with
  small holes on tmpfs (Ubuntu 26.04's `/tmp`) four expansion tests read the
  wrong bytes and failed. The test helper now reads unbuffered.

### Release of 24 October 2026

### Distributions
* Nine more distributions tested and starting (QEMU/OVMF, official ISOs,
  checksums or signatures verified): AlmaLinux 10.2, CentOS Stream 10, Alpine
  Linux 3.24, BlackArch 2023.04, Kali Linux 2026.3 (live), Parrot OS 7.4,
  MX Linux 25.3, antiX 26.1, Slackware 15.0. The full matrix of 54 ISOs:
  53 start, Debian netinst stays blocked; 15 start with Secure Boot on.
* New `antix` adapter for antiX and MX Linux (live-init skips loop devices
  in its own scan; `bdev=/dev/loop200` names the virtual medium).
* New `slackware` adapter: the installer starts; the image's page explains
  the one manual step (source device `/dev/loop200`).
* Older archiso (BlackArch 2023) gets `archisodevice=/dev/loop200`: its udev
  never created the by-label link for the loop device.
* Kernels up to 5.10 (antiX's) start with `efi=noruntime`; they crashed in a
  UEFI runtime call after kexec.
* New `best-effort` method: 64-bit Linux images of a family Distro Buny has
  no adapter for start with their own boot entry and the virtual medium, after
  a confirmation (badge *Best effort*). Tested with Alpine Linux.
* Names, versions and icons for antiX, BlackArch, MX, Parrot and Slackware
  (monogram badges where no official mark exists).
* Lab: the test matrix strips colour codes from serial output and knows the
  antiX/MX and Slackware success signs.
* `tools/development/buny-lab-image` builds lab images from ISO folders (how
  the 54-ISO test image was made).

### Safety
* Images that install without asking (Ubuntu `autoinstall`, kickstarts that
  partition disks, Debian preseeding) are marked *Erases a disk* and need a
  confirmation, like self-installing images. A kickstart on the image is read
  to decide; Fedora IoT's (which only names its source) is not flagged.
* Growing the data partition only ever writes to the drive the computer was
  started from.
* The pre-init re-announces the virtual medium to the distribution's udev a
  few times after attaching it. The final checks showed that udev, while it is
  still starting, can miss the first announcement and never create
  `/dev/disk/by-label/<label>` (Void failed once in a parallel run).
* The ISO and boot-configuration parsers were run against thousands of
  corrupted images under AddressSanitizer and UndefinedBehaviorSanitizer
  (`tests/robustness/fuzz_probe.py`).

### Drive
* The disk layout puts the ESP first and DISTROBUNY last (still entry 1 of
  the partition table), with a growable exFAT file system.
* *Use the whole drive*: on a stick bigger than the image, the menu grows
  DISTROBUNY to the end of the stick after a confirmation; power-cut safe,
  finished automatically at the next start.

### Release
* The complete corresponding source of every third-party component (Ubuntu
  source packages) ships in the release folder, so no written offer is
  needed.
* `SHA256SUMS` lists every file of the release and the unpacked image;
  `buny-build release` and `buny-build publish` verify the folder (checksums
  quoted in documents, unlisted files, unfinished text, third-party source).
  This fixes the stale source checksum in the exported 1.0.0 `SOURCE-CODE.txt`.
* Copyright notices name "the Distro Buny authors".
* The user guide and the design overview are PDFs built from
  `documentation/release/`; the release also carries the Distro Buny logo,
  mark and the 1.0.1 trailer (`Branding/`).
* `buny-build publish` copies a release into a folder and verifies the copy;
  it accepts only files that are identical to the release's own.
* Removed a placeholder URL from the runtime's `/etc/os-release`; added a
  top-level `COPYING` (GPL-3.0) to the source.
* Secure Boot is explained correctly: Distro Buny starts with Secure Boot on
  or off; with it on, only the Ubuntu family can be started.

## 1.0.0 (internal, October 2026)

First complete build: UEFI boot chain (shim, GRUB, Ubuntu's signed kernel),
the graphical menu, the virtual medium and kexec hand-off, 13 distribution
adapters, the installer and updater, and the starter image. 44 of 45 test
ISOs started in QEMU/OVMF; 15 with Secure Boot enabled.
