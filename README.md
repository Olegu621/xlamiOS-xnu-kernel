# xlamiOS

**Apple's iOS 16 running on the POCO F6 (`peridot`, Qualcomm SM8630 / Snapdragon 8s Gen 3).**

This project boots Apple's open-source XNU kernel (xnu-8796, the iOS 16
generation) on the POCO F6 and runs the **unmodified iOS 16.7 userland** on
top of it: launchd, the system daemons, backboardd, SpringBoard and the
stock apps.

Nothing is flashed. The stock bootloader is started once with
`fastboot boot` of a temporary image, the kernel and the iOS root volume are
mapped into RAM, and the session lives for as long as the machine stays up.

> Experimental bring-up, not a usable phone OS. Apple's security model is
> mostly switched off, most radios are missing, and the iOS root volume lives
> in the `userdata` partition, so it **erases Android's user data**.

## Status

| Area | State |
| --- | --- |
| Boot | Stock bootloader → `fastboot boot` of our shim → XNU → iOS. No partition is written |
| CPU | 8 cores via PSCI, 16 KiB pages, GICv3, timers, SMP bring-up |
| Memory | The upper DDR bank, with a private DMA arena for the display and the SPI engine |
| Storage | Own UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write |
| iOS userland | Real iPhone X 16.7 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`); CoreAnimation renders in software; frames are copied into the bootloader's framebuffer. Presented to iOS as an iPhone X panel (1125×2436) |
| GUI | backboardd, SpringBoard, lock screen, home screen, Control Center, notification center, Settings |
| Touch | Goodix BerlinD (9916R) on the serial engine, read over bit-banged SPI, delivered as HID digitizer events |
| Buttons | Power, volume up/down (PMIC over SPMI) as HID consumer keys |
| Panel rails | AVDD and IOVDD voted through RPMh, reset sequenced on the panel's GPIO |
| Diagnostics | Every boot leaves a persistent console log on the device; the screen can be captured as a PNG from the kernel |

## Not there yet

- No GPU driver: everything is drawn by the CPU.
- No Wi-Fi, Bluetooth, cellular, audio, camera, sensors or Secure Enclave.
- No App Store and no Apple ID.
- The touch path exists but has not been exercised by a finger yet.

## How it boots

```
ABL (stock bootloader, unlocked)
  └─ fastboot boot shim-boot.img        bare metal: sets up the MMU, loads the
       └─ XNU (kernel.development.sweet)   kernel and the iOS root, then jumps
            └─ iOS: launchd → backboardd → SpringBoard
```

The shim is a small bare-metal ELF that lives in the repository under
`shim/`. It enables the MMU, reads the kernel and the iOS root volume over
UFS, places them where XNU expects them, and hands over. Everything after
that is ordinary XNU.

Every session is bounded: a boot argument arms a timer that resets the
machine back into the bootloader, and the kernel log is written to the
`userdata` partition so it can be pulled afterwards.

## Repository layout

| Path | What it is |
| --- | --- |
| `xnu/` | The XNU port: `patches/` is the patch series applied to xnu-8796, `out-*/` holds the built kernels |
| `shim/` | The bare-metal loader that starts XNU |
| `tools/` | Host tools: ADT and payload builders, the boot-cycle runner, the log and screen decoders, the symbol checker |
| `research/` | Working notes, device dumps, kernel trees and the host test harnesses (not tracked in git) |
| `NOTES.md` | The running log of the bring-up: hardware facts, every experiment and its result, and the next concrete action |
| `AGENTS.md` | House rules for agents and contributors working on this repository |
| `docs/` | Longer-form documentation |
| `site/` | The static project page |

## Building

The kernel is built by GitHub Actions (`xnu/patches/**` triggers a run):

```
git push
gh run list -R <account>/xlamiOS
gh run download <run-id> -n xnu-kernel -D xnu/out-0107
```

The artifact contains `kernel.development.sweet` and a `SHA256SUMS` file that
must verify before the kernel is used.

A payload is the kernel plus the device tree plus the boot arguments:

```
python3 tools/mkpayload.py xnu/out-0107/kernel.development.sweet \
    shim/out/peridot-0092-mac.adt shim/out/payload.bin --cmdline '...'
bash shim/build.sh out/payload.bin
```

`tools/peridot-cycle.py` then runs a whole session unattended: it backs up
the BCB, reboots into fastboot, boots the image temporarily, waits for the
kernel to reset itself and pulls the console log back.

## Working on it

`NOTES.md` is the source of truth. It records what the hardware actually
answered, what was guessed, and what the next experiment is. Read it before
changing anything.

House rules are in `AGENTS.md`: keep the device dumps out of git, never
flash a partition for bring-up, confirm every destructive step, and update
the handoff in `NOTES.md` after each milestone.

## License

The kernel patches are derivative works of XNU and are distributed under the
Apple Public Source License 2.0, like XNU itself. The shim, the tools and
the documentation are part of this project. No Apple firmware, kernels or
other proprietary files are stored here; everything Apple-side is fetched
from Apple's own servers by the tools in `tools/`.
