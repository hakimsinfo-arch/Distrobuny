# Distro Buny — Developer Guide

Read `ARCHITECTURE.md` first; this guide is about working on the code.

## 1. Source tree

```
source/
  common/          util, strbuf, log (ring buffer + kmsg), sha256, json (parser + writer)
  filesystem/      iso9660 reader, block-device enumeration + GPT, exFAT extent mapper
  iso-manager/     identification rules, ISO probing, catalog + index
  boot-adapters/   boot entry model, GRUB interpreter, sd-boot/syslinux parsers, adapters, planner
  boot/            buny-init (PID 1), pre-init, hand-off (kexec), cpio writer, extents, IPC, module loader
  security/        firmware / Secure Boot / lockdown state, firmware-setup request
  interface/       the graphical menu (display, gfx, path, font, image, input, theme, icons, app, render)
  tools/           bunyctl (command-line front end to the C core)
  third_party/     stb_truetype.h (public domain / MIT)
  python/distrobuny/
    image/         GPT, FAT32, exFAT writers, disk layout, write "pieces"
    build/         pipeline, components (pinned debs), runtime, esp, assets, image_build
    install/       drive discovery/assessment, writer (installer back end)
    updater/       sources, HTTP (resumable), verification (checksums, OpenPGP), engine
    qemu/          VM driver (QMP, OVMF, fw_cfg), test matrix
configuration/
  boot/components.json             pinned boot packages (+ test tools)
  distributions/distributions.conf identification and folder rules
  distributions/compatibility.json lab results shown in the menu (generated)
  updates/sources.json, keys/      update sources and pinned vendor keys
assets/            fonts (Inter), distribution icons (Simple Icons), generated branding
tools/             build/, maintenance/, development/ command-line tools
tests/             unit/ (C, Python), integration/, boot/, compatibility/runs/
```

The C core (`common`, `filesystem`, `iso-manager`, `boot-adapters`) is
linked into three programs: `buny-ui` (runtime), `buny-ui-host` (desktop
build of the same interface) and `bunyctl`. `buny-init` and `buny-preinit`
are static runtime programs. `buny-preinit` deliberately depends on nothing
but libc, because it runs inside other distributions' initrds.

## 2. Conventions

* C11 with GNU extensions, `-Wall -Wextra`, warnings are bugs. Tabs, kernel
  style braces, 120 columns.
* Memory: `xmalloc`/`xstrdup` abort on exhaustion (the runtime cannot do
  anything useful without memory). Every function that returns allocated
  memory says so in its header. `ptrvec` and `strbuf` are the containers.
* Untrusted input (ISO contents, configuration files on ISOs, the data
  partition) is parsed only in unprivileged code (`buny-ui`, `bunyctl`).
  Every parser is bounded: depth limits, step limits for the GRUB
  interpreter, size limits on reads.
* The privileged side (`buny-init`) accepts only small, validated requests:
  paths are resolved with `realpath` and must stay inside the data mount,
  state writes are whitelisted file names.
* Python: standard library only, type hints, `BuildError` / `InstallError`
  / `UpdateError` for messages meant for users.
* Messages to users say what happened and what to do, in plain words. Error
  codes go in logs, not in sentences.

## 3. Building and testing quickly

```bash
make -j"$(nproc)" host test                 # C core, bunyctl, desktop interface, C unit tests
build/bin/bunyctl probe "Iso layout/Desktop/Arch/archlinux-x86_64.iso"   # identify + plan one ISO
build/bin/bunyctl scan --json "Iso layout" | less                         # catalog as the menu sees it
python3 tests/integration/test_ui_navigation.py                          # interface, offscreen
```

To see the whole chain in a VM, use a small image and refresh only its ESP
after changes:

```bash
tools/build/buny-build image --profile dev
tools/build/buny-build runtime && tools/build/buny-build esp
tools/build/buny-build refresh-esp --image output/images/distrobuny-1.0.1-dev.img
tools/development/buny-test-matrix --image output/images/distrobuny-1.0.1-dev.img --filter archlinux
```

### Runtime options

The runtime reads `buny.*` options from the kernel command line (GRUB's
`$buny_extra`, or `/distrobuny/options.cfg` on the ESP). For tests it also
reads QEMU's fw_cfg item `opt/distrobuny/cmdline`, so the image
never has to change:

| Option | Effect |
| --- | --- |
| `buny.lab=1` | machine-readable `BUNY-EVENT` lines on the serial console; serial console added to started systems |
| `buny.autoboot=/path.iso` | start that image without interaction |
| `buny.dryrun=1` | prepare and validate hand-offs but never kexec |
| `buny.medium=dm` | build the virtual medium through device-mapper even for contiguous files |
| `buny.debug=1` | verbose logging |
| `buny.shell=1` | root shell instead of the interface |
| `buny.expandcrash=N` | reset the machine after N writes of a drive expansion the user started (power-cut test); the next start finishes it |

`buny-test-matrix --lab-option buny.medium=dm` passes extra options.

### Debugging a hand-off

* `bunyctl stage --image IMG --iso HOSTISO --iso-path /x.iso --preinit build/runtime-bin/buny-preinit --out DIR`
* `bunyctl expand --image IMG [--check] [--stop-after N]` grows DISTROBUNY to the end of an image file, using the
  runtime's own code (`source/filesystem/expand.c`). Enlarge the file first (`truncate -s 64G copy.img`), as a
  bigger stick would be. `--stop-after N` simulates a power cut after N writes. It only accepts regular files: on
  a real drive the menu does this, with the user's confirmation.
  writes exactly what
  the runtime would load (kernel, combined initrd with overlay, command line,
  `medium.conf`). You can then boot it directly with
  `qemu-system-x86_64 -kernel … -initrd … -append …`, no menu involved.
* In lab mode the pre-init mirrors its messages to the console
  (`distrobuny-medium: …`). They are always in the kernel log (`dmesg`).
* The menu's log page (*System information* → *Log*) shows the runtime's ring
  buffer.

## 4. Adding a distribution family

1. **Identify it.** Add a section to
   `configuration/distributions/distributions.conf`:

   ```ini
   [archlinux]
   name = Arch Linux
   icon = archlinux
   vendor = arch
   variant = Install medium
   match.label = ^arch_
   match.file = ^archlinux-
   rolling = yes
   ```

   `match.<source>`, `version.<source>` and `variant.<source>` take POSIX
   extended regular expressions (case-insensitive). The first group is
   extracted for version and variant. Sources: `label` (ISO volume label),
   `file` (file name), `info` (`.disk/info` and similar identity files),
   `treeinfo` (`.treeinfo`), `app` (ISO application id) and `title` (boot
   menu titles). `bunyctl probe` shows what matched.
2. **Boot it.** In `source/boot-adapters/adapters.c`, write:
   * `detect(probe)`: return a score (0 = not mine). Look at boot arguments
     (`any_arg_prefix`) and files in the ISO (`iso_exists`).
   * `prepare_virtual(probe, entry, ctx, &blocker)`: adjust the cloned
     entry so the initrd finds its medium on `/dev/loop200` (or by the
     label/UUID that device carries). Remove the image's own ISO-file
     arguments (`remove_iso_file_args`).
   * `prepare_native(...)`: optional. Pass `p->iso_path` with the family's
     own ISO-file parameter, after `path_ok_for_cmdline()`. Otherwise set
     `*blocker` to a sentence explaining why it is impossible.

   Add the adapter to the table at the end of the file; order matters only
   for equal scores.
3. **If the system does something destructive** (installs to a disk
   without asking), set `plan->warning` in `plan.c` so the menu asks for
   confirmation.
4. **Test it.** Add an ISO fixture case to `tests/unit/c/test_isoboot.c` if
   the configuration syntax is unusual. Boot a real ISO with
   `buny-test-matrix`, and add a success pattern to `SUCCESS` in
   `source/python/distrobuny/qemu/matrix.py` if the system prints none of
   the generic markers. Never weaken a pattern so far that a failed boot
   could match.
5. **Icon.** Add an SVG from Simple Icons (CC0) to
   `assets/distro-icons/simple-icons/` and map it in
   `source/python/distrobuny/build/assets.py` (`SIMPLE_ICON_MAP`), or add a
   monogram (`MONOGRAMS`).

## 5. Adding an update source

Add an entry to `configuration/updates/sources.json`:

```json
{
  "id": "ubuntu-desktop",
  "name": "Ubuntu Desktop",
  "installed": "^ubuntu-(?P<version>\\d{2}\\.\\d{2}(?:\\.\\d+)?)-desktop-amd64\\.iso$",
  "series": {"index": "https://releases.ubuntu.com/", "regex": "href=\"(\\d{2}\\.\\d{2})/\"", "policy": "track"},
  "checksums": "https://releases.ubuntu.com/{series}/SHA256SUMS",
  "pick": "^ubuntu-(?P<version>\\d{2}\\.\\d{2}(?:\\.\\d+)?)-desktop-amd64\\.iso$",
  "signature": {"type": "detached", "url": "{checksums}.gpg", "key": "ubuntu-cdimage.asc"},
  "download": "https://releases.ubuntu.com/{series}/{file}"
}
```

* `installed` recognises the file on the drive and extracts its version.
* `series` (optional) discovers release directories; `policy: track` keeps
  LTS installations on LTS series.
* The signing key goes into `configuration/updates/keys/` and its full
  fingerprint into `keys` in `sources.json`. Check the fingerprint against
  the vendor's own documentation. Signatures by any other key are rejected.
* Signature types: `detached` (checksum file + `.gpg`/`.sig`) and
  `clearsigned` (signed checksum file, as Fedora uses).

Test with `tools/maintenance/buny-update --root <folder> check`. It only
reads metadata; nothing is downloaded until `update` is approved.

## 6. Interface development

* Layout and drawing live in `render.c` (one `layout_*` and one `draw_*` per
  screen); behaviour lives in `app.c` (`activate`, `handle_key`,
  `handle_pointer`). Items carry a stable `key` so focus and animations
  survive relayout.
* All sizes go through `S(a, px)`, the scale factor for the current display
  and the *Interface size* setting. Test at 800×600 and 4K
  (`test_small_and_large_displays`).
* Colours come only from the theme tokens (`theme.c`), so light and dark
  stay consistent.
* `app_emit()` events are the interface's test API. Add one when you add a
  screen or dialog, and test the flow in `tests/integration/`.

## 7. Safety rules for contributors

* Nothing in this repository may write to a block device except
  `buny-install`, and only after its checks and the typed confirmation.
* Tests use image files and VMs. VMs get qcow2 overlays; images are never
  modified by tests.
* Do not add Ventoy, Easy2Boot, YUMI or similar code, binaries or formats
  (Ventoy plugin JSON, etc.).
