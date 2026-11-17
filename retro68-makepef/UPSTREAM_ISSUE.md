# Text for a Retro68 issue / pull request (not yet sent)

Title: MakePEF: exported-symbol hash table, section file alignment, mainSection for libraries

Building a PowerPC shared library with more than a few exports showed four problems in
`PEFTools/MakePEF.cc` (checked against upstream commit a4a389c, 10 Nov 2026). Patches:
`MakePEF_final.diff` (1-3) and `MakePEF_b21_mainsection.diff` (4). `pefcheck.py` is an
independent validator written from Apple's "Mac OS Runtime Architectures" ch. 8 and
`PEFBinaryFormat.h`.

1. **Export hash table.** With the original tool only some exports are reachable through
   the hash table: 5 of 10, 3 of 33, 0 of 300 in our tests (the SharedLibrary sample has 2
   exports and never hits it). Fix: slot function, chain ordering and counts as Apple
   specifies.
2. **Hash of names with bytes >= 0x80.** `hash()` read names as signed `char`; now
   `unsigned char` with unsigned arithmetic.
3. **File alignment of sections.** Code is used in place from the file, so the FILE OFFSET
   of the code and unpacked-data sections must be 16-byte aligned for AltiVec constants
   (GCC puts 16-byte vector constants in both). The patch pads section offsets to 16.
4. **mainSection without an entry point.** A library with no main got `mainSection = 1`,
   `mainOffset = 0xFFFFFFFF`. `PEFBinaryFormat.h`: "Section containing the main symbol,
   -1 => none". Fix: -1 / 0 when there is no entry point.

Verified with libraries of 1 to 300 exports (names up to ~265 characters) and with a
648 KB library (14 exports, AltiVec code) that loads and runs under Mac OS 9.2.2 on QEMU
and on a Mac mini G4.

Retro68 is GPL; the diffs here are offered under Retro68's own licence terms.
