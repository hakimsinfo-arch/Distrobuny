# Distro Buny — Third-Party Software Notices

**Release:** 1.0.1  
**Document status:** Prepared from the project's existing licensing records; final release verification remains outstanding.

## 1. Overview

Distro Buny is free and open-source software. Its original project code is identified in the project documentation as licensed under the **GNU General Public License, version 3 or later (GPL-3.0-or-later)**. See the repository's `COPYING` file.

Distro Buny also incorporates third-party software, fonts, and distribution icons. These materials remain subject to their respective licenses, copyright notices, and applicable trademark rights. Distro Buny's project license does not replace those terms.

The Distro Buny 1.0.1 starter image does **not** bundle Linux distribution ISO files. Users obtain their own ISO files from the relevant publishers.

## 2. Principal Third-Party Components

The following versions and licenses are recorded in Distro Buny's `LICENSES.txt` and `SOURCE-CODE.txt` for release 1.0.1. They describe the project's records and are subject to verification against the final distributed binaries and source archives.

| Component | Recorded version | Declared license |
| --- | --- | --- |
| UEFI shim | 15.8 | BSD-2-Clause |
| GNU GRUB | 2.12 | GPL-3.0-or-later |
| Linux kernel and modules | 7.0.0 | GPL-2.0-only |
| BusyBox | 1.36.1 | GPL-2.0-only |
| GNU C Library (glibc) | 2.39 | LGPL-2.1-or-later |
| stb_truetype | 1.26 | MIT or public domain |
| Inter typeface | — | SIL Open Font License 1.1 |
| Distribution icons | Various | Individual terms; see icon attribution records |

The project's notices also identify exFAT up-case table data reproduced from exfatprogs. See `LICENSES.txt` and `SOURCE-CODE.txt` for its documented origin.

## 3. License Texts and Copyright Notices

The project records third-party license texts and copyright notices at:

- `build/esp/distrobuny/LICENSES.txt`
- `build/esp/distrobuny/licenses/`
- The release package's **Licenses and source code** folder, including its `licenses/` directory

The notices supplied with each component remain applicable. This summary is not a substitute for those legal texts.

## 4. Corresponding Source Code

The release's `SOURCE-CODE.txt` identifies the Ubuntu source packages and exact versions corresponding to the included shim, GRUB, Linux kernel, BusyBox, and glibc binaries. It also identifies Distro Buny's original source archive, checksums, and rebuild documentation.

The release records describe the following materials in the **Licenses and source code** folder:

- `SOURCE-CODE.txt` — package versions, source information, and instructions
- `third-party/` — recorded upstream and Ubuntu source package files
- `licenses/` — applicable license texts and notices
- `distrobuny-1.0.1-source.tar.xz` — Distro Buny project source archive

Users should consult `SOURCE-CODE.txt` and `documentation/BUILD_INSTRUCTIONS.md` for source and build information. Any redistribution of the starter image must satisfy the applicable licenses' corresponding-source requirements; the presence of this summary alone does not fulfill them.

## 5. Distribution Icons and Trademarks

Distribution icon sources and declared licenses are documented in:

- `assets/distro-icons/ATTRIBUTION.md`
- `build/esp/distrobuny/ICON-ATTRIBUTION.md`

Distribution names and logos remain associated with their respective owners. Their use to identify ISO files does not imply endorsement, sponsorship, or affiliation.

**Outstanding review:** Some icon entries currently show `CC0-1.0 (Simple Icons)` when no individual license is specified in Simple Icons metadata. The collection's CC0 status must not be treated as proof that every individual icon is CC0. These entries, individual license obligations, and relevant trademark guidelines require further review.

## 6. Outstanding Release Verification

Before declaring third-party licensing compliance complete, the project should:

1. Confirm the public release actually contains the stated license texts, source archives, and corresponding build materials.
2. Review LGPL obligations for glibc, which `SOURCE-CODE.txt` describes as statically linked into `buny-init`, `buny-ui`, and `buny-preinit`.
3. Verify the licensing and attribution requirements for individual distribution icons and review applicable trademark conditions.
4. Recheck component versions, source files, and notices whenever the release is rebuilt or updated.

**Contact:** distrobuny.dev@proton.me

*This file is an informational index of existing project records, not a legal compliance certification.*
