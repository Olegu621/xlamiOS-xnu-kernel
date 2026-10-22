# Source review, 2026-10-22

This is the short implementation checklist from reviewing the projects linked
in the OpenCode conversation and searching for additional ports. Hardware
results for this phone remain in `NOTES.md`.

| Source | Inspected code / contract | Decision for peridot |
| --- | --- | --- |
| [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel), `8e61187098f2` | `xnu/files/bsd/dev/redwood_goodix.c`: `goodix_read`, `goodix_write`, `goodix_poll`, GPIO SPI | Keep the already working GPIO transport. `goodix_poll` writes **one zero byte** to the event address; this exposed our missing ACK payload, fixed in0111. The frame is copied before ACK. Keep peridot's measured pins, two rails, report address and 16-times coordinate units. |
| [NativeHidInput](https://github.com/UniXNU/NativeHidInput), `28f9b463b48e` | `NativeHidInput.cpp`: digitizer collection, finger identity, edge-origin mask | Our existing HID collection already carries finger identity and the edge-origin flag.0111 supplies the missing committed physical panel geometry. Its binary/vtable hooks target a different IOHIDFamily ABI and are not compatible by address or slot number. |
| [NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay), `8666a9950c96` | `NativeLegacyDisplay.cpp`: swap validation, surface bounds/stride, frame retirement; timer-based vsync | Use as the reference when reviewing our swap/presentation contract. Its GPU-copy retirement depends on its own surface and fence services. Preserve the currently measured peridot framebuffer and logical canvas. |
| [NativeAdreno](https://github.com/UniXNU/NativeAdreno), `26220f1ab79f` | README and driver source layout: GMU, CP, BOs, fences, cache and native4KiB SMMU | A substantial future GPU reference, with actual sheng hardware results reported upstream. Adreno740 board setup and 4KiB GPU translation need a peridot/Adreno735 audit before any hardware access. CPU16KiB pages and GPU page tables are separate contracts. |
| [NativeGeniSpi](https://github.com/UniXNU/NativeGeniSpi) | `GeniSpi.c`: `transfer_dma`; `GeniSpi.h`: cache/physical-buffer callbacks, `allow_fifo_disabled` | Optional future bus acceleration reference. Its direct SE DMA is distinct from GPI DMA. The FIFO override is an attempt flag, not evidence of a working IF_DISABLE=1 path on this phone. GPIO SPI remains the hardware-proven path here. |
| [UniXNU](https://github.com/UniXNU/UniXNU), [NX729J](https://github.com/2713385115/UniXNU-nx729j) | Build/boot boundaries, board profiles, ADT and staged device acceptance | Adopt tools and acceptance criteria where useful. Their boot chain, page-size patches, SoC clock addresses and firmware selection require matching the actual target. |
| [UniXNU-peridot](https://github.com/kvfsrl/UniXNU-peridot) | `board.toml`, ADT/identity generation | Compare board data and boot identity. A profile is not evidence of running touch, SpringBoard or GPU. |
| [xnu-darwin-peridot](https://github.com/hoshikv/xnu-darwin-peridot), `d9e3f9330698` | `0130` Goodix framing; `0127` HID dispatch through command gate; USB repeater/reset patches | Framing agrees with our measured chip. HID command-gate serialization is a useful follow-up review. USB includes several experimental, superseding sequences: validate PMIC ownership, clocks, PHY and controller state before porting any one sequence. |
| [peridot-xnu-build](https://github.com/kvfsrl/peridot-xnu-build) | Build workflow | Automation reference; it builds the sibling patch series, rather than providing another touch implementation. |
| [ios-topaz](https://github.com/MakrSas/ios-topaz), `e4772911e116` | `0950` release contacts on empty reports; published GPU/userland patch series | Our Goodix decoder already releases every contact on a valid count-zero frame. The FocalTech register format is different. GPU/userland pieces merit ABI and hardware review after touch; their Adreno610 register programming is not a peridot driver. |
| [Snapintosh](https://github.com/Acelogic/Snapintosh) | Published architecture and hardware requirements: EL2 monitor, stock macOS/AuxKC, Adreno Metal/AIR compiler | Useful architectural and shader/compiler reference. Its X2 Elite board, EL2 proxy and macOS ABI differ from this EL1 iOS16 port. It is not a drop-in boot chain for the phone. |

## Next acceptance steps

1. Boot0111 temporarily with `peridotspi=7`; verify probe checksums, measured
   HID geometry and poller heartbeat in the saved log.
2. Verify a real finger produces paired down/up events and correctly placed
   taps/swipes in iOS. Record observations separately from source inspection.
3. Check idle operation, release after an empty report and multiple contacts.
4. Review HID work-loop serialization and USB device access. A working USB
   control/log link would reduce dependence on recovery cycles.
5. Start GPU work with a peridot hardware/firmware/SMMU inventory and an
   isolated command/fence probe before connecting it to the compositor.

Preserve upstream notices and licenses when bringing source files into the
patch series. Local source snapshots and device captures stay under ignored
`research/diagnostics-20261022/`.
