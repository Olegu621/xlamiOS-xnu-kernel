Source: https://github.com/UniXNU/AdrenoFirmware
Commit:40fdb8ea87606612ac6f1e80bbb49955d0ce4051
License: BSD-2-Clause-Patent (LICENSE retained).

AdrenoFirmware.c/.h are unmodified. Only the portable GMU image planner is
vendored here; it performs no MMIO, allocation, firmware execution or SCM call.
The HFI and vote libraries remain upstream until the hardware path needs them.
tools/peridot-gmu-plan.c exercises this parser against peridot's measured VMA
and additionally validates metadata preallocation ranges (opaque upstream).
