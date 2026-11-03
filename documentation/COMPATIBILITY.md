# Distro Buny — Compatibility

Every result below comes from booting one lab image (54 ISO files: the 45 of
the original test collection and 9 added for 1.0.1, built with the 1.0.1
runtime on 24 October 2026) in QEMU 8 with OVMF UEFI firmware (q35, KVM,
4 GiB RAM, the drive attached as a USB stick) through the complete chain:
firmware → shim → GRUB → Distro Buny runtime → menu → kexec → the
distribution. The image was built with `tools/development/buny-lab-image`, the
tests were driven by `tools/development/buny-test-matrix`;
evidence (serial logs, screenshots, QEMU command lines, per-image
`result.json`) is in `tests/compatibility/runs/final-1.0.1` and
`final-1.0.1-sb`. The Debian installer, blocked in that run, was fixed on
28 October 2026; the fixed runtime was then re-checked with 24 images of
every family that was still available, with and without Secure Boot (runs
`fix-1028-*`, see "Re-check of 28 October 2026" below).

The 9 new ISOs were downloaded from their projects' official servers and
checked before use: SHA-256 lists for Alpine, AlmaLinux, CentOS Stream, Kali,
MX Linux and antiX, Parrot's signed hash list, BlackArch's published SHA-1,
and the OpenPGP signature of Slackware (key EC56 49DA 401E 22AB FA67 36EF 6A44
63C0 4010 2233).

**PASS** means the distribution's own user space was reached *through the
virtual medium*: a login prompt, a graphical or multi-user target, the live
installer started (Anaconda "Starting installer", YaST, Agama, subiquity, the
Slackware installer's first question) or the system's init running from the
ISO (Guix's Shepherd, antiX/MX live-init handing over). Showing the Distro
Buny menu is never counted as a distribution boot. The times are seconds from
power-on (firmware, Distro Buny menu, hand-off and the distribution's own
start) to that point.

## Summary

| | UEFI | UEFI + Secure Boot |
| --- | --- | --- |
| PASS | **54** | **15** |
| refused by Secure Boot: kernel not signed | — | 20 |
| refused by Secure Boot: kernel signed by a CA this machine does not trust | — | 19 |
| blocked (no working method yet) | 0 | 0 |
| FAIL | 0 | 0 |

* Without Secure Boot, every image reaches user space: Debian's netinst
  installer since the fixes of 28 October 2026, and the 9 added for 1.0.1 (AlmaLinux, CentOS
  Stream, Alpine, BlackArch, Kali, Parrot, MX Linux, antiX, Slackware).
* With Secure Boot (Microsoft keys enrolled, as shipped on PCs), every
  Canonical-signed kernel starts: Ubuntu and all its flavours, Ubuntu
  Server, Linux Mint and Zorin OS. Fedora, Bazzite, openSUSE, AlmaLinux,
  CentOS Stream, MX Linux and Debian sign their kernels with their own CAs, which
  only their own shims trust. Arch, BlackArch, Manjaro, Nobara, Pop!\_OS,
  Void, Gentoo, NixOS, Guix, Kali, Parrot, antiX, Alpine and Slackware ship
  kernels the runtime treats as unsigned. The menu predicts both cases from
  the kernel's signature and the machine's trusted keys, and marks those
  images **Needs Secure Boot off** (`SECURITY.md` §2).

## All images

| Image | Adapter | UEFI | UEFI + Secure Boot |
| --- | --- | --- | --- |
| EndeavourOS 2026.08.15<br>`/Desktop/Arch/EndeavourOS_Titan-Nova-2026.08.15.iso` | archiso | **PASS** (39 s) | refused: unsigned kernel |
| Arch Linux 2026.10.01<br>`/Desktop/Arch/archlinux-x86_64.iso` | archiso | **PASS** (33 s) | refused: unsigned kernel |
| CachyOS 2026.08.09<br>`/Desktop/Arch/cachyos-desktop-linux-260809.iso` | archiso | **PASS** (42 s) | refused: unsigned kernel |
| Manjaro 26.1.0<br>`/Desktop/Arch/manjaro-kde-26.1.0-260812-linux71.iso` | miso | **PASS** (31 s) | refused: unsigned kernel |
| Manjaro 26.1.0<br>`/Desktop/Arch/manjaro-xfce-26.1.0-260812-linux71.iso` | miso | **PASS** (27 s) | refused: unsigned kernel |
| Omarchy 4.0.6<br>`/Desktop/Arch/omarchy-4.0.6.iso` | archiso | **PASS** (24 s) | refused: unsigned kernel |
| Fedora COSMIC Atomic 44<br>`/Desktop/Fedora/Fedora-COSMIC-Atomic-ostree-x86_64-44-1.7.iso` | anaconda | **PASS** (42 s) | refused: kernel signed by an untrusted CA |
| Fedora IoT 44<br>`/Desktop/Fedora/Fedora-IoT-ostree-44-20260427.0.x86_64.iso` | anaconda | **PASS** (40 s) | refused: kernel signed by an untrusted CA |
| Fedora KDE Plasma 44<br>`/Desktop/Fedora/Fedora-KDE-Desktop-Live-44-1.7.x86_64.iso` | dracut-live | **PASS** (30 s) | refused: kernel signed by an untrusted CA |
| Fedora Kinoite 44<br>`/Desktop/Fedora/Fedora-Kinoite-ostree-x86_64-44-1.7.iso` | anaconda | **PASS** (42 s) | refused: kernel signed by an untrusted CA |
| Fedora Budgie Atomic 44<br>`/Desktop/Fedora/Fedora-Onyx-ostree-x86_64-44-1.7.iso` | anaconda | **PASS** (42 s) | refused: kernel signed by an untrusted CA |
| Fedora Sway Atomic 44<br>`/Desktop/Fedora/Fedora-Sericea-ostree-x86_64-44-1.7.iso` | anaconda | **PASS** (42 s) | refused: kernel signed by an untrusted CA |
| Fedora Silverblue 44<br>`/Desktop/Fedora/Fedora-Silverblue-ostree-x86_64-44-1.7.iso` | anaconda | **PASS** (41 s) | refused: kernel signed by an untrusted CA |
| Fedora Workstation 44<br>`/Desktop/Fedora/Fedora-Workstation-Live-44-1.7.x86_64.iso` | dracut-live | **PASS** (44 s) | refused: kernel signed by an untrusted CA |
| Linux Mint 22.3<br>`/Desktop/Linux mint/linuxmint-22.3-cinnamon-64bit.iso` | casper | **PASS** (29 s) | **PASS** (26 s) |
| Linux Mint 22.3<br>`/Desktop/Linux mint/linuxmint-22.3-xfce-64bit.iso` | casper | **PASS** (25 s) | **PASS** (28 s) |
| MX Linux 25.3<br>`/Desktop/MX-25.3_Xfce_x64.iso` | antix | **PASS** (15 s) | refused: kernel signed by an untrusted CA |
| Gentoo 20261018<br>`/Desktop/Not friendly/Gentoo-amd64-minimal-20261018T124555Z.iso` | dracut-live | **PASS** (42 s) | refused: unsigned kernel |
| Arch Linux 2026.10.01<br>`/Desktop/Not friendly/archlinux-x86_64.iso` | archiso | **PASS** (34 s) | refused: unsigned kernel |
| GNU Guix System 1.5.0<br>`/Desktop/Not friendly/guix-system-install-1.5.0.x86_64-linux.iso` | guix | **PASS** (18 s) | refused: unsigned kernel |
| NixOS 26.05<br>`/Desktop/Not friendly/nixos-graphical-26.05.12400.da6dc0e7b260-x86_64-linux.iso` | nixos | **PASS** (24 s) | refused: unsigned kernel |
| Void Linux 20250202<br>`/Desktop/Not friendly/void-live-x86_64-20250202-base.iso` | dracut-live | **PASS** (23 s) | refused: unsigned kernel |
| Parrot OS 7.4<br>`/Desktop/Parrot-home-7.4_amd64.iso` | debian-live | **PASS** (38 s) | refused: unsigned kernel |
| Zorin OS 18.1<br>`/Desktop/Zorin-OS-18.1-Core-64-bit.iso` | casper | **PASS** (30 s) | **PASS** (31 s) |
| antiX 26.1<br>`/Desktop/antiX-26.1_x64-full.iso` | antix | **PASS** (15 s) | refused: unsigned kernel |
| BlackArch Linux 2023.04.01<br>`/Desktop/blackarch-linux-netinst-2023.04.01-x86_64.iso` | archiso | **PASS** (25 s) | refused: unsigned kernel |
| Debian 13.7.0<br>`/Desktop/debian-13.7.0-amd64-netinst.iso` | debian-installer | **PASS** (22 s, 28 Oct) | refused: kernel signed by an untrusted CA (28 Oct) |
| Nobara 44<br>`/Desktop/gaming/Nobara-44-Official-2026-10-13.iso` | dracut-live | **PASS** (36 s) | refused: unsigned kernel |
| Bazzite 2026.10.13<br>`/Desktop/gaming/bazzite-nvidia-open-stable-live-amd64.iso` | dracut-live | **PASS** (36 s) | refused: kernel signed by an untrusted CA |
| Bazzite 2026.10.13<br>`/Desktop/gaming/bazzite-stable-live-amd64.iso` | dracut-live | **PASS** (44 s) | refused: kernel signed by an untrusted CA |
| Pop!_OS 24.04<br>`/Desktop/gaming/pop-os_24.04_amd64_generic_28.iso` | casper | **PASS** (52 s) | refused: unsigned kernel |
| Pop!_OS 24.04<br>`/Desktop/gaming/pop-os_24.04_amd64_nvidia_28.iso` | casper | **PASS** (56 s) | refused: unsigned kernel |
| Kali Linux 2026.3<br>`/Desktop/kali-linux-2026.3-live-amd64.iso` | debian-live | **PASS** (39 s) | refused: unsigned kernel |
| openSUSE Leap 16.0<br>`/Desktop/open suse/Leap-16.0-offline-installer-x86_64.install.iso` | kiwi | **PASS** (22 s) | refused: kernel signed by an untrusted CA |
| openSUSE Tumbleweed Snapshot20261020<br>`/Desktop/open suse/openSUSE-Tumbleweed-DVD-x86_64-Snapshot20261020-Media.iso` | linuxrc | **PASS** (52 s) | refused: kernel signed by an untrusted CA |
| Slackware 15.0<br>`/Desktop/slackware64-15.0-install-dvd.iso` | slackware | **PASS** (16 s) | refused: unsigned kernel |
| Edubuntu 26.04.1 LTS<br>`/Desktop/ubuntu/edubuntu-26.04.1-desktop-amd64.iso` | casper | **PASS** (53 s) | **PASS** (54 s) |
| Kubuntu 26.04.1 LTS<br>`/Desktop/ubuntu/kubuntu-26.04.1-desktop-amd64.iso` | casper | **PASS** (57 s) | **PASS** (57 s) |
| Lubuntu 26.04.1 LTS<br>`/Desktop/ubuntu/lubuntu-26.04.1-desktop-amd64.iso` | casper | **PASS** (45 s) | **PASS** (46 s) |
| Ubuntu 26.04.1 LTS<br>`/Desktop/ubuntu/ubuntu-26.04.1-desktop-amd64.iso` | casper | **PASS** (47 s) | **PASS** (45 s) |
| Ubuntu Budgie 26.10<br>`/Desktop/ubuntu/ubuntu-budgie-26.10-desktop-amd64.iso` | casper | **PASS** (52 s) | **PASS** (47 s) |
| Ubuntu MATE 26.10<br>`/Desktop/ubuntu/ubuntu-mate-26.10-desktop-amd64.iso` | casper | **PASS** (51 s) | **PASS** (47 s) |
| Ubuntu Unity 24.04.4 LTS<br>`/Desktop/ubuntu/ubuntu-unity-24.04.4-desktop-amd64.iso` | casper | **PASS** (28 s) | **PASS** (26 s) |
| Ubuntu Cinnamon 26.04.1 LTS<br>`/Desktop/ubuntu/ubuntucinnamon-26.04.1-desktop-amd64.iso` | casper | **PASS** (45 s) | **PASS** (50 s) |
| Ubuntu Kylin 26.10<br>`/Desktop/ubuntu/ubuntukylin-26.10-desktop-amd64.iso` | casper | **PASS** (54 s) | **PASS** (51 s) |
| Ubuntu Studio 26.04.1 LTS<br>`/Desktop/ubuntu/ubuntustudio-26.04.1-desktop-amd64.iso` | casper | **PASS** (64 s) | **PASS** (68 s) |
| Xubuntu 26.10<br>`/Desktop/ubuntu/xubuntu-26.10-desktop-amd64.iso` | casper | **PASS** (48 s) | **PASS** (47 s) |
| AlmaLinux 10.2<br>`/Servers/AlmaLinux-10.2-x86_64-boot.iso` | anaconda | **PASS** (35 s) | refused: kernel signed by an untrusted CA |
| CentOS Stream 10<br>`/Servers/CentOS-Stream-10-latest-x86_64-boot.iso` | anaconda | **PASS** (31 s) | refused: kernel signed by an untrusted CA |
| Fedora Server 44<br>`/Servers/Fedora-Server-dvd-x86_64-44-1.7.iso` | anaconda | **PASS** (36 s) | refused: kernel signed by an untrusted CA |
| Alpine Linux 3.24.3<br>`/Servers/alpine-standard-3.24.3-x86_64.iso` | best-effort | **PASS** (15 s) | refused: unsigned kernel |
| openSUSE Leap Micro<br>`/Servers/openSUSE-Leap-Micro.x86_64-Default-SelfInstall.iso` | kiwi | **PASS** (13 s) | refused: kernel signed by an untrusted CA |
| openSUSE MicroOS Snapshot20261020<br>`/Servers/openSUSE-MicroOS-DVD-x86_64-Snapshot20261020-Media.iso` | linuxrc | **PASS** (55 s) | refused: kernel signed by an untrusted CA |
| Ubuntu Server 26.04.1 LTS<br>`/Servers/ubuntu-26.04.1-live-server-amd64.iso` | casper | **PASS** (30 s) | **PASS** (30 s) |

## By family

| Family (adapter) | Images | Medium found through | Notes |
| --- | --- | --- | --- |
| Ubuntu, flavours, Mint, Zorin, Pop!\_OS (`casper`) | 17 | `live-media=/dev/loop200` | Ubuntu Server's subiquity runs too. Mint ends its command line with `--`; Distro Buny keeps kernel parameters in front of it. |
| Arch, EndeavourOS, CachyOS, Omarchy, BlackArch (`archiso`) | 6 | ISO UUID/label on the loop device; BlackArch's 2023 archiso gets `archisodevice=/dev/loop200` | `rootdelay=30` gives udev time on slow USB sticks |
| Manjaro (`miso`) | 2 | `misolabel` | |
| Fedora live, Bazzite, Nobara, Void, Gentoo (`dracut-live`) | 7 | `root=live:CDLABEL=` | |
| Fedora Atomic, IoT, Server, AlmaLinux, CentOS Stream (`anaconda`) | 9 | the ISO's `inst.stage2=hd:LABEL=` | RHEL uses the same installer (not tested: it needs a Red Hat account) |
| openSUSE Leap 16, Leap Micro (`kiwi`) | 2 | `root=install:CDLABEL=` | Leap Micro is a **self-installing image**: see below |
| openSUSE Tumbleweed and MicroOS DVD (`linuxrc`) | 2 | `install=hd:/?device=loop200` | 349 MiB installer initrd (see "kexec handover" below) |
| NixOS (`nixos`) | 1 | `root=LABEL=` | |
| GNU Guix (`guix`) | 1 | file-system UUID | |
| Kali Linux, Parrot OS (`debian-live`) | 2 | `live-media=/dev/loop200` | Kali's separate *installer* ISO is a Debian installer (same method as Debian's; not tested) |
| MX Linux, antiX (`antix`) | 2 | `bdev=/dev/loop200` (live-init's own scan skips loop devices) | antiX 26.1's kernel 5.10 gets `efi=noruntime`, see below |
| Slackware (`slackware`) | 1 | the installer runs from its initrd | when `setup` asks for the source: CD/DVD → manual → custom → `/dev/loop200` (shown on the image's page; this step is not driven by the lab) |
| Alpine Linux (`best-effort`) | 1 | its initramfs finds the medium by itself | |
| Debian netinst (`debian-installer`) | 1 | `/dev/loop200` as a CD drive, loop driver from the image's pool | see "Debian installer" below |

## Families without an adapter (best effort)

A 64-bit Linux image that none of the adapters recognises is still offered:
the `best-effort` adapter starts its own boot entry, unchanged, with the
virtual medium attached. The medium carries the image's volume label and
UUID, which is what most initramfs implementations search for. Families that
only look at real CD drives stop at their own prompt; nothing is written. The
menu marks such images **Best effort** and asks before starting them. Tested
with a real distribution: **Alpine Linux 3.24.3** (PASS). antiX and MX Linux
did *not* work this way (their scan skips loop devices), which is why they
got their own adapter.

## Old kernels and UEFI runtime services

antiX 26.1 ships kernel 5.10. Started by kexec from the 7.0 runtime kernel,
it called a UEFI runtime service through a null pointer while loading
certificates (`load_uefi_certs`) and panicked. Distro Buny starts kernels up
to 5.10 with `efi=noruntime`; antiX then boots normally (PASS). An installer
started this way cannot add a firmware boot entry. Newer kernels (MX Linux's,
Slackware's 5.15) keep runtime services.

## Known limitations

* **Debian live images** use `live-boot`, like Kali and Parrot (tested);
  no Debian live ISO itself was in the test collection, so that is **not
  tested**. The Debian *DVD* uses the same installer as the tested netinst
  image but was not tested either.
* **Self-installing images (openSUSE Leap Micro "SelfInstall").** The image
  boots and immediately offers to erase a disk. In the test VM it offered the
  Distro Buny drive itself, because the image cannot recognise the drive it
  was started from. Distro Buny marks such images **Erases a disk** and asks
  for confirmation before starting them. Nothing was written in the test
  (the prompt was not answered, and VMs only use throw-away snapshots).
* **Secure Boot** limits as above. Enrolling a vendor's CA with the MOK
  manager should let that vendor's kernels start. This follows from how the
  kernel builds its trusted keys, but was **not tested**.
* **Legacy BIOS** is not supported.

## Debian installer

debian-installer's CD initrd has no loop driver and no `iso-scan`, and
`cdrom-detect` only mounts what udev knows as a CD drive or disk. Distro Buny
therefore takes the loop driver for the installer's own kernel from the
image's pool (`loop-modules-<release>-di` udeb), the pre-init extracts it with
the installer's BusyBox and loads it, and a udev rule tags `/dev/loop200` as
a CD drive. The installer then finds the medium, loads its components from
it and continues as from a real CD.

Tested with Debian 13.7.0 netinst: the lab boots it to network detection
(3 of 3 runs, `fix-1028-debian-*`, and the re-check below), and the graphical
installer, started without any lab console as a user starts it, reached
"Configure the network - Please enter the hostname". Two lab details: with
two consoles (`console=tty0 console=ttyS0`) the installer starts two copies
that corrupt each other's package database, so the lab gives it only the
serial console, and the text installer with language and keyboard answered
(both come before the medium search). Users get the image's own entries
unchanged.

## Re-check of 28 October 2026

The fixed runtime, on one lab image with every ISO still available (24 files,
one Arch ISO in two folders; openSUSE and NixOS images were no longer
available):

| | Result |
| --- | --- |
| UEFI (`fix-1028-regress`) | **24 PASS**: Arch Linux (2 folders), EndeavourOS, CachyOS, Omarchy, BlackArch, Manjaro KDE and Xfce, Fedora Workstation, Void, Gentoo, Guix, Ubuntu Desktop and Server 26.04.1, Zorin OS, Debian netinst, Kali, Parrot, MX Linux, antiX, Slackware, AlmaLinux, CentOS Stream, Alpine |
| UEFI + Secure Boot (`fix-1028-regress-sb`, sample) | Ubuntu Desktop, Ubuntu Server and Zorin OS **PASS**; Debian and Fedora refused (untrusted CA), Arch refused (unsigned), as designed |

## Robustness checks

| Check | Result | Evidence |
| --- | --- | --- |
| ISO stored in 3 out-of-order fragments (FAT chain), Void Linux | PASS: assembled with device-mapper, presented as `/dev/loop200` | `tests/boot/test_fragmented_boot.py`, `tests/compatibility/runs/fragmented/` |
| Device-mapper path forced for contiguous files (Arch, Void, Ubuntu, Leap 16) | PASS | `tests/compatibility/runs/dm-path*/` |
| Drive written by `buny-install create` (16 GiB stand-in drive, Arch) | PASS | `tests/compatibility/runs/install-e2e-create/` |
| Drive written by `buny-install write` (dev image onto a 32 GiB stand-in drive, backup GPT moved to the end) | 5 PASS, 1 BLOCKED (Debian; a 1.0.0 run, before the Debian fix) | `tests/compatibility/runs/install-e2e-write/` |
| File names with spaces (`/Desktop/Not friendly/...`) | PASS for all 5 images in that folder | `full-1` |

## History of the results (fixed defects)

The first full run (`full-1`) had 37 PASS, 7 FAIL and 1 BLOCKED. All 7 were
investigated:

* **openSUSE Tumbleweed and MicroOS DVD — real defect, fixed.**
  `kexec_file_load` returned `EADDRNOTAVAIL`. The runtime kernel enables
  *kexec handover* by default (`CONFIG_KEXEC_HANDOVER_ENABLE_DEFAULT=y`),
  which confines the next kernel's segments to small scratch areas, and the
  349 MiB installer initrd did not fit. The runtime now boots with `kho=off`,
  and the menu explains this error in plain words should it ever occur.
* **Linux Mint (2) — test harness defect, fixed.** Mint's command line ends
  with `--`; the lab's serial-console parameters were appended after it and
  therefore went to init instead of the kernel, so the harness saw no output
  although Mint reached its desktop (screenshot in `full-1`). Lab parameters
  now go in front of `--`/`---`.
* **Guix, Ubuntu Server, openSUSE Leap Micro — success not recognised,
  fixed.** All three had reached their installer (screenshots in `full-1`).
  These systems print none of the generic markers, so specific ones were
  added (Shepherd's root file system, subiquity's serial interface, KIWI's
  installer prompt). All recorded evidence was re-checked against the
  stricter markers.

`full-2-fixes` re-ran the 7 images on the fixed runtime: 7 PASS.
`final-smoke*` re-checked one image per family on the final released image.
