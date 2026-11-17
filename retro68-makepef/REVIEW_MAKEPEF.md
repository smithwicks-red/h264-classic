# MakePEF review (patches: export hash, 16-byte sections)

Spec: Apple "Mac OS Runtime Architectures" ch. 8 for the hash, slot and exponent functions (https://developer.apple.com/library/archive/documentation/mac/runtimehtml/RTArch-100.html) and the section header (https://developer.apple.com/library/archive/documentation/mac/runtimehtml/RTArch-92.html). Section header rules:
- `alignment` is in-memory alignment (power of 2), not file alignment; CFM ignores it under System 7.
- Read-only sections run in place, so the file offset decides their alignment (container is 16 bytes, 4 KB if file-mapped).
- Code and unpacked data "should be at least 16-byte aligned" in the file.

## Verdicts
1. **Export hash patch: correct but incomplete.**
   - `hashIndex`, the sort and the counts match PEFHashTableIndex. Chains are contiguous. The power loop matches Apple's function.
   - Defect: `hash()` used signed `char` (Apple: UInt8), so names with bytes ≥ 0x80 got wrong keys. A UTF-8 export (café_ü) could not be found. Also signed-overflow UB.
   - **Fixed** (`unsigned char`, uint32_t arithmetic). Output for AVCDecLib.pef is byte-identical, so no rebuild is needed. Old binary: `toolchain/bin/MakePEF.prev`; old source: `MakePEF.cc.d20`.
2. **Section alignment patch: correct.**
   - Offsets are rounded consistently and padded. Lengths and `defaultAddress` are unchanged.
   - Relocations are section-relative; the replay ends at the data length (0x7A20). Data is unpacked.
   - `ppc_h264_altivec.o`: vector constants are in .text (RO csect, 2^4), with another 2^4 csect in .data. Both need the fix; the patch covers both.

## Pre-existing quirk (proposed fix, not applied)
With no entry point MakePEF still writes `mainSection=1`, `mainOffset=0xFFFFFFFF`, so CFM reports a garbage main address. Fix: set mainSection only if `entry != 0xFFFFFFFF`. If applied, AVCDecLib must be rebuilt and re-tested.

## Validator (review/pefcheck.py, written independently)
| File | Result |
|---|---|
| AVCDecLib.pef (8 exports, p0) | OK, 8/8 reachable, offsets 0x80/0x92080/0x99AA0, 9231 reloc instructions |
| AVCSpike2.pef (10, p1) | OK, 10/10 |
| Stress test 1, 2, 9, 10, 16, 17, 33, 100, 300 (p0–p5, names up to about 265 chars) | all OK |
| Non-ASCII name (6 exports) | FAIL before the fix, OK after |
| Original MakePEF on 10/33/300 exports | FAIL: 5/10, 3/33, 0/300 reachable (confirms bug 1) |

## Residual risks
- The cfrg offset must stay a multiple of 16 (4096 with VM file-mapping). AVCDecLib uses kZeroOffset; recheck if the PEF moves into a component file.
- Only checked on the host; not re-run under CFM on OS 9.

## Upstream
- autc04/Retro68 HEAD a4a389c (2026-11-10): `MakePEF.cc` matches our `.orig`, so all issues are still there.
- The SharedLibrary sample (2 exports, power 0) cannot hit bug 1; any library with ≥10 exports would.
- Worth one upstream PR: slot fix, unsigned char, 16-byte offsets, mainSection.
