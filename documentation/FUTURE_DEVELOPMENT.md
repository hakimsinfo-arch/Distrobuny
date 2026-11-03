# Distro Buny — Future Development

What should come next, in order of value to users. Each item names the
constraint it removes and how to approach it. Items are based on test
results and known limits (`TEST_RESULTS.md`, `COMPATIBILITY.md`), not on
guesses.

## 1. Secure Boot for every signed distribution

**Today:** under Secure Boot, the Ubuntu runtime kernel's kexec verification
trusts Canonical's keys, the UEFI `db` and MOK. Distributions whose kernels
are signed by their own CA, which only their own shim knows, are refused
(see `COMPATIBILITY.md` for the measured list). Unsigned kernels are refused
by design.

**Options, strongest first:**

1. **Enrol distribution CAs into MOK, guided by the menu.** Distro Buny
   already ships the MOK manager. The menu could offer "Trust Fedora's
   kernels on this computer". It would stage the vendor's CA certificate
   (shipped on the ESP, fingerprint-pinned like the updater's keys) for
   `MokManager`, which then asks the user at the next start. The trust
   decision stays with the user and the firmware's physical-presence prompt.
   MOK keys feed the kernel's platform/machine keyring, so kexec accepts
   those kernels.
2. **Chain-load the distribution's own shim + GRUB from the ISO** instead of
   kexec, for Secure Boot only. The firmware then verifies the
   distribution's own chain, and no new trust is needed. The cost: the
   system's GRUB must find its files, which needs a firmware-visible view of
   the ISO (an EFI block-I/O driver exposing the ISO extent, signed, so it
   would need option 3) or per-distribution GRUB configuration.
3. **A Distro Buny shim signed by Microsoft.** It lets Distro Buny
   ship its own runtime kernel and a signed EFI driver. This needs the
   shim-review process and an organisational key-management programme (HSM,
   revocation, SBAT). It is the long-term route for a commercial product.

## 2. Debian installer images

**Done** (1.0.1, fixes of 28 October 2026): the image's own `loop-modules`
udeb is extracted by the pre-init and the virtual medium is tagged as a CD
drive (`ARCHITECTURE.md` §3.4). Left to do: a native ISO-file strategy (the
CD initrd has no `iso-scan`; the image's `iso-scan` udeb could be added the
same way) and tests with a Debian DVD and with installers built on d-i
(Kali's installer ISO).

## 3. Self-installing images that know their origin

KIWI OEM self-install images (Leap Micro) start correctly but cannot
recognise the drive they came from, so they may offer the Distro Buny drive
as the installation target. The menu now warns and asks for confirmation.
Better: pass the image's own device-exclusion parameters. KIWI supports
device filters in its boot options; research which ones apply at boot time,
then exclude the data partition's disk by its persistent ID.

## 4. More hardware coverage

* **Real-hardware lab.** All boots so far ran in QEMU/OVMF (q35, KVM).
  Next: a small fleet (Intel/AMD laptops, a desktop with NVIDIA graphics, an
  older 2013-era UEFI board, a Mac with an Intel processor) using
  `buny-test-matrix`'s serial/screenshot evidence through IP-KVMs.
* **Firmware quirks.** Some firmware only boots the first partition or
  requires the ESP to be partition 1. The ESP already comes first on the
  disk; if entry 1 of the table matters in practice, swap the two entries as
  a build option (the runtime finds partitions by name, and the expansion
  only requires DISTROBUNY to be last on the disk).
* **Low-memory machines.** kexec needs the distribution's kernel and initrd
  in memory twice for a moment (the 349 MiB openSUSE DVD initrd needs about
  1.1 GiB in total). Stream the initrd straight from the ISO into the memfd
  to remove one copy, and report the memory needed before trying.
* **Legacy BIOS.** The layout reserves room for a BIOS boot partition and the
  pinned `grub-pc-bin` package is already verified. What is missing is GRUB's
  BIOS core image wiring and tests with SeaBIOS.
* **ARM64 (aarch64) UEFI.** The C code is portable. It needs an arm64
  runtime kernel, a shim/GRUB chain for arm64, and adapters for arm64 ISOs.

## 5. Native ISO-file boot where it is missing

`linuxrc` (openSUSE DVD) and Guix have no native ISO-file path today
(Guix's initrd has no such option at all). The virtual medium covers both,
so this is a robustness extra, not a gap.

## 6. Update subsystem

* 22 trusted update sources covered 24 of the 45 original reference images. Add sources
  for Pop!\_OS, Zorin, Bazzite, Nobara, CachyOS, EndeavourOS, Manjaro,
  NixOS, Void, Gentoo and Guix where vendors publish signed checksums. Do not
  add sources that publish only unsigned checksums.
* A graphical front end (and Windows/macOS builds) for `buny-update` and
  `buny-install`.
* Optional: let the menu verify an ISO's SHA-256 against the vendor's
  signed list before starting it.

## 7. User experience

* Persistence for live systems (casper-rw, Fedora overlay) stored as files
  on the data partition, created from the menu.
* Localisation of the interface (all strings are in two C files) and an
  on-screen keyboard for touch-only devices.
* Accessibility: high-contrast theme, larger focus rings, audible feedback.
* Favourites and recently used images on the start page.

## 8. Engineering

* Fuzz the untrusted-input parsers (ISO 9660 reader, GRUB interpreter,
  syslinux/sd-boot parsers, JSON) with libFuzzer. They are bounded,
  unit-tested and passed a sanitizer run against 3,000 corrupted images, but
  no coverage-guided fuzzing has been done yet.
* Pin the host toolchain (container image) so reproducible builds also hold
  across build machines, not only across runs on one machine.
* Continuous integration running the unit, integration and dev-image VM
  tests on every change.
