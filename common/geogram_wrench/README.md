# geogram_wrench — vendored Wrench VM

Upstream: <https://github.com/jingoro2112/wrench> — **version 7.2.2**, MIT
(`license.txt`). Vendored as upstream ships it: the whole interpreter is two
files, `wrench.h` and `wrench.cpp`, taken verbatim from `src/` so a version
bump is a straight file replacement.

**Do not edit `wrench.cpp` or `wrench.h`.** Every build-time choice is a
`-D` in `CMakeLists.txt`, precisely so an upgrade does not have to be a merge.
The station-facing host — the task, the allocator, the natives, the loader —
is a separate component and does not live here.

## Why this and not the alternatives

Measured, not assumed. On this project's own toolchain, in the exact
configuration shipped here (COMPACT plus the four safety defines), the VM costs
**31,809 bytes of `.text`, 412 of `.data` and 24 of `.bss`**, and its undefined symbols
are plain C: `malloc`/`free`, libm, `snprintf`, `strtol`, ctype. No libstdc++,
no exceptions, no RTTI. `wr_setGlobalAllocator()` redirects every allocation,
which is what lets script memory be confined to a capped PSRAM pool instead of
competing with the radios for internal DRAM.

## What it is not

**Not a sandbox.** Wrench is a bytecode interpreter with no memory isolation
from the firmware. A script cannot reach anything it has not been handed a
native for, so **the native binding surface is the entire attack surface**, and
malformed bytecode is a memory-safety problem rather than a script error. The
format carries only a CRC (`WR_ERR_bad_bytecode_CRC`) — an integrity check that
is trivially forged. Bytecode is therefore accepted **only** with a valid
signature, checked before the VM is handed a single byte, the same rule
`xprs_ota` applies to firmware.

## The size property is load-bearing, so it is tested

`test/test_wrench_host.sh` compiles the VM for the host in the shipping
configuration and **fails the build if anything pulls in libstdc++**. One
upstream bump that adds an `operator new` would cost roughly 40 KB of flash
and nothing else in the tree would notice — the same way a whole block of
`sdkconfig` once went missing for months because no test covers a Kconfig
value (`docs/esp32.md`).
