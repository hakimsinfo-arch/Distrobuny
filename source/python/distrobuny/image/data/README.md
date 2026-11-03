# Image writer data

`exfat-upcase-table.bin` — the *recommended up-case table* defined in the
Microsoft exFAT File System Specification, section 7.2.5.1, in its
compressed form (5836 bytes, table checksum `0xE619D30D`).  This is the table
written by Windows and by `mkfs.exfat`; using it byte-for-byte keeps Distro Buny
volumes interoperable with every exFAT implementation.  The bytes were taken
from exfatprogs 1.2.2 `mkfs/upcase.c` (GPL-2.0-or-later), which reproduces the
specification's table, and are verified against the published checksum each
time the image writer loads them.
