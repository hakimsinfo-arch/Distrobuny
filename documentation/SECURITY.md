# Distro Buny — Security

This document covers what Distro Buny protects, how, and where its limits are.
It describes the implementation as built and tested; test evidence is in
`TEST_RESULTS.md` and `COMPATIBILITY.md`.

## 1. What is protected, and against what

| Asset | Threat | Main defences |
| --- | --- | --- |
| The computer's boot integrity | running unauthorised code before the OS | UEFI Secure Boot is respected end to end, never bypassed (§2) |
| The user's other disks | an installer or tool writing the wrong disk | read-only runtime, installer safety rules, warnings for self-installing images (§5, §7) |
| The ISO collection | corruption by the runtime, failed updates | data partition mounted read-only, read-only virtual medium, verified atomic updates (§4, §6) |
| The runtime itself | malicious or malformed ISO files | parsing in an unprivileged process, bounded parsers, validated requests (§3) |
| Downloads | tampered or substituted ISO images | HTTPS + vendor OpenPGP signatures with pinned fingerprints + checksums (§6) |

Out of scope: an attacker with physical write access to the Distro Buny drive
who also controls the firmware (they can replace anything). Distro Buny
limits what such an attacker gains on a Secure Boot machine (§2.4), but a USB
drive is not a tamper-proof medium.

## 2. Secure Boot

### 2.1 The chain

| Stage | Binary | Signed by | Verified by |
| --- | --- | --- | --- |
| 1 | `EFI/BOOT/BOOTX64.EFI` = shim 15.8 (Ubuntu `shim-signed 1.58`) | Microsoft UEFI CA | firmware (`db`) |
| 2 | `grubx64.efi` (Ubuntu `grub-efi-amd64-signed 1.202.5`) | Canonical | shim (embedded Canonical CA, SBAT) |
| 3 | `distrobuny/vmlinuz` (Ubuntu kernel 7.0.0-38-generic) | Canonical | GRUB through shim's protocol |
| 4 | the chosen distribution's kernel | its vendor | the running kernel's `kexec_file_load` signature check (lockdown) |

The packages for stages 1–3 are pinned by version and SHA-256
(`configuration/boot/components.json`) and used unmodified. Distro Buny adds
no unsigned EFI binaries and asks for no firmware or MOK changes to start.

With Secure Boot on, the runtime kernel enters **lockdown (integrity
mode)**. From then on the kernel itself enforces that `kexec_file_load` only
loads kernels whose signature chains to a key it trusts. Those keys are the
kernel's built-in keys (Canonical), the firmware's `db` (platform keyring)
and Machine Owner Keys enrolled through shim. Distro Buny's code cannot
switch this off; the hand-off code passes the kernel to the kernel and
reports the result.

### 2.2 Measured behaviour

All test images were booted in QEMU/OVMF with Secure Boot enforced and
Microsoft's keys enrolled (the 54-image run `final-1.0.1-sb` of 1.0.1; the
Debian installer re-checked on 28 October 2026):

* **Canonical-signed kernels start.** Ubuntu and its flavours, Linux Mint,
  Zorin OS and Ubuntu Server reached user space.
* **Kernels signed by other vendors' CAs are refused** (`ENOKEY`, "Required
  key not available"): Fedora and its derivatives, openSUSE. Those CAs are
  embedded only in the vendors' own shims, which this boot path does not use.
  The same applies to AlmaLinux, CentOS Stream, MX Linux and Debian.
* **Unsigned kernels are refused** (`ENODATA`): the Arch family, Manjaro,
  Void, Gentoo, NixOS, Guix, Nobara, Pop!\_OS, Kali, Parrot, antiX, Alpine,
  Slackware.

Exact per-image results are in `COMPATIBILITY.md`.

### 2.3 How the menu handles it

* The runtime reports the firmware's Secure Boot and Setup Mode state, the
  kernel lockdown mode and the subjects of the keys the kernel trusts
  (from `/proc/keys`, read by the privileged init).
* For every image the menu inspects the kernel's Authenticode signature
  (signer and issuing CA). With Secure Boot on, an image whose kernel is
  unsigned, or signed by a CA the running kernel does not trust, is marked
  **Needs Secure Boot off** before the user tries it. The detail page says
  why.
* If a start is refused anyway, the error is translated ("Secure Boot is on
  and this system's kernel is not signed…") and the dialog offers
  *Restart into firmware settings* (UEFI `OsIndications` boot-to-firmware-UI).
* Users who manage their own keys can enrol a vendor's CA with the MOK
  manager (*Esc* at start → *Advanced options* → *Enroll a Secure Boot key*).
  shim then makes that key available to the kernel's trust store and such
  kernels start.

### 2.4 Deliberate non-features and limits

* **No bypass.** Distro Buny does not use exploits, unsigned shims, "shim
  bypass" tricks, policy overrides, or kernels built without lockdown. It
  does not try to load untrusted kernels some other way: no `kexec_load`
  without signatures (lockdown forbids it), no chain-loading of unsigned EFI
  binaries.
* **The initramfs is not signed.** As in a standard Ubuntu installation,
  GRUB verifies the kernel but not the initrd. Someone who can write the
  drive's ESP can change Distro Buny's runtime programs. Under Secure Boot
  they still run under lockdown and can only start kernels the machine
  trusts. A Distro Buny-owned shim and signed unified kernel images would
  close this (`FUTURE_DEVELOPMENT.md` §1).
* **No own Secure Boot key.** Distro Buny has no Microsoft-signed shim of
  its own, so it relies on Canonical's chain. That is why only kernels
  trusted by Canonical's kernel (plus `db`/MOK) start under Secure Boot.

## 3. Privilege separation in the runtime

```
buny-init  (PID 1, root)                       buny-ui  (uid 100, no new privileges)
  hardware, drivers, mounts                     display, input, menu
  kexec, power, ESP state writes      ◄─IPC─►   parses ISO files, GRUB scripts,
  validates every request                       distribution configuration
```

* The interface process runs as user 100 with `PR_SET_NO_NEW_PRIVS`. It
  cannot load modules, mount, write disks or call kexec.
* All untrusted content is parsed there: ISO 9660 structures from any file on
  the drive, the ISOs' GRUB/systemd-boot/SYSLINUX configuration, identity
  files. The GRUB interpreter is a sandboxed evaluator. It has no I/O except
  reading the ISO, a step limit, and depth and size limits. The other parsers
  are bounded as well.
* The privileged side accepts a few message types over a `SOCK_SEQPACKET`
  socket pair (`ARCHITECTURE.md` §3.2). Boot requests must name an `.iso`
  inside the data partition after `realpath` resolution. State writes accept
  two whitelisted file names. Kernel and initrd arrive as memfds; under
  Secure Boot the running kernel checks the system kernel's signature on the
  exact bytes it loads, so nothing can be swapped after the check.
* Before a hand-off, init verifies the virtual medium. The ISO's volume label
  and its first and last 64 KiB, read through the computed extent map, must
  match the file. A wrong map can therefore never present other data to the
  next system.

## 4. Data integrity

* The data partition is mounted **read-only** by the runtime. The virtual
  medium is a **read-only** loop device (or read-only device-mapper table),
  so a distribution cannot modify the ISO file or the partition through it.
* The runtime writes only small state files on the ESP (`settings.json`,
  `last-boot.json`), atomically.
* The installer, the updater and the image builder never modify ISO files.
  Updates add the new file next to the old one and replace atomically (§6).

## 5. Destructive images

Some images install themselves onto a disk as soon as they start (KIWI OEM
"SelfInstall" images such as openSUSE Leap Micro). Started from a multiboot
drive, they cannot recognise the drive they came from. In testing, Leap
Micro offered to erase the Distro Buny drive itself. Distro Buny detects such
images (`cdinst`, `root=install:`), marks them **Erases a disk**, shows a
warning on the image page and requires an explicit *Start anyway* (the
default choice is *Cancel*). The image's own confirmation prompt remains as a
second barrier.

Unattended installers get the same treatment, because they can erase a
computer's internal disk without any prompt: Ubuntu `autoinstall`, Debian
preseeding (`auto=true`, `priority=critical` with a preseed), and Anaconda
kickstarts that partition (`clearpart`, `zerombr`, `autopart`, `part`,
`reqpart`). A kickstart stored on the image is read to decide this (Fedora
IoT's only names its ostree source and is not flagged); one from the network
or the initrd cannot be read and counts as unattended. Images of an unknown
family (*Best effort*, `COMPATIBILITY.md`) also need a confirmation.

Growing the data partition (ARCHITECTURE.md §2) writes only to the drive the
computer was started from: the disk of the ESP that GRUB named on the kernel
command line, which must also hold DISTROBUNY. A DISTROBUNY partition found
any other way is never changed.

## 6. Update integrity

`tools/maintenance/buny-update`:

* HTTPS only (plain HTTP is refused; `localhost` is allowed for tests).
* The vendor's checksum list must carry a valid OpenPGP signature by a key
  whose **full fingerprint is pinned** in `configuration/updates/sources.json`.
  The fingerprints were checked against the vendors' published values. Keys
  live in `configuration/updates/keys/`. Verification runs in a throw-away
  GnuPG home, so the user's keyring is neither used nor changed, and the
  signature must be `VALIDSIG` by the pinned primary key.
* The downloaded file's SHA-256 must match the signed list before it is
  used. A mismatch deletes the download; the original stays untouched.
* Downloads go to a staging area on the Distro Buny drive only (the tool
  refuses folders that are not Distro Buny volumes) and resume after
  interruptions.
* Nothing is replaced without approval. Every replacement is logged in
  `/.distrobuny/updates.log`.

The tests (`tests/unit/python/test_updater.py`) cover an impostor key, a
corrupted download, an interrupted download, plain-HTTP refusal and
non-volume refusal.

## 7. Installer safety

`tools/maintenance/buny-install` never writes anything the user did not
name and confirm:

* Drives holding the running system (`/`, `/boot`, `/boot/efi`, `/usr`,
  `/var`, `/home`, swap, including through LVM/LUKS/RAID stacking) are
  **always** refused. No option overrides this.
* Drives with mounted or held file systems, write protection, 4K logical
  sectors, too little space or no medium are refused. Internal (non-USB/SD)
  disks need `--allow-fixed-disk`.
* The device is opened with `O_EXCL`, an exclusive kernel claim. It fails if
  anything is mounted and prevents mounting while the install runs. Its
  identity (device number, size, model, serial, sector size) is checked
  again after opening. The user must type the drive's name.
* Everything written is read back and compared, which also detects
  counterfeit "fake capacity" drives. The partition table is written last.
* Root privileges are needed only for real block devices, and the tool never
  escalates by itself.

## 8. Supply chain

* Third-party binaries: shim, GRUB, the Linux kernel and modules, BusyBox,
  all from the Ubuntu archive, pinned by SHA-256 and unpacked unmodified.
  The test tools (exfatprogs, mtools) are pinned the same way and are not
  shipped.
* Third-party source: `stb_truetype.h` v1.26 (public domain/MIT), vendored
  with its SHA-256 recorded. Fonts: Inter (SIL OFL 1.1). Icons: Simple Icons
  (CC0). See `REPORT.md` §24.
* Builds are reproducible: identical inputs give a bit-identical image
  (`SOURCE_DATE_EPOCH`), which lets anyone check a published image against
  the source.
* No Ventoy, Easy2Boot or YUMI code, binaries, plugins or formats are used.

## 9. Known risks and recommendations

| Risk | Status / mitigation |
| --- | --- |
| Parsers of untrusted data are not continuously fuzzed | bounded, unit-tested, unprivileged; a sanitizer robustness run (`tests/robustness/fuzz_probe.py`: 3,000 corrupted ISOs and boot configurations under AddressSanitizer and UndefinedBehaviorSanitizer, 24 October 2026) found no crash, hang or sanitizer report; coverage-guided fuzzing is still planned (`FUTURE_DEVELOPMENT.md` §8) |
| Lab "Verified" badges match label and size, not a hash | documented; the updater verifies cryptographically; in-menu hash verification planned |
| Lab automation options (`buny.autoboot`, `buny.dryrun`) can be set by anyone who can write the ESP | equivalent to physical access; the options cannot weaken Secure Boot |
| exFAT has no permissions: anyone with the drive can replace ISOs | inherent to a shareable USB drive; Secure Boot still limits what can start |

## 10. Reporting

Report security issues privately to the maintainers before publishing them.
A dedicated security contact will be published with the project's download
page (an open decision of the project owner). Include the Distro Buny
version (*System information* page), the firmware and the image involved.
