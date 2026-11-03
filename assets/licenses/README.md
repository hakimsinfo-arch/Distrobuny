# Licence texts shipped on every drive

`buny-build esp` copies these files to `distrobuny/licenses/` on the boot
partition, together with the Debian `copyright` files of the shipped Ubuntu
packages and of the build host's C library (`libc6`).

| File | Applies to |
| --- | --- |
| `GPL-3.0.txt` | Distro Buny's own code, GRUB 2 |
| `GPL-2.0.txt` | the Linux kernel and modules, BusyBox |
| `LGPL-2.1.txt` | the GNU C Library, statically linked into buny-init, buny-ui and buny-preinit |
| `OFL-1.1-Inter.txt` | the Inter typeface |
| `stb_truetype-LICENSE.txt` | stb_truetype (MIT or public domain) |

The GPL and LGPL texts are the Free Software Foundation's, as distributed in
Debian/Ubuntu `/usr/share/common-licenses`.  shim's BSD-2-Clause terms are in
its `copyright` file.
