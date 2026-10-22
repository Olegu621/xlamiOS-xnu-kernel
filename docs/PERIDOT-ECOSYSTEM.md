# The peridot ecosystem

Inventory of the public work on Apple's XNU on Qualcomm Android phones, as it
relates to the POCO F6 (`peridot`, SM8635/SM8630, Snapdragon 8s Gen 3).

Survey date 2026-10-22. The `websearch` tool was unavailable (HTTP 403 from the search
provider), so every web claim below comes from a page fetched directly: the GitHub REST
API, `raw.githubusercontent.com`, `git.kernel.org` and `unixnu.com`.

Local clones read: `/tmp/opencode/{unixnu-peridot,xnu-darwin-peridot,nx729j,unixnu,genispi,hackios}`.
**`/tmp/opencode/peridot-xnu-build` does not exist on this machine** (`ls` exits 2); the
remote repository exists and was read over the GitHub API instead. In `/tmp/opencode/unixnu`
the `drivers/` and `kits/` submodule directories are empty (not initialised), so those
modules were read from their own repositories.

## 1. Inventory

| Repository | Owner | Target | Boot model | iOS | Licence | Current state |
| --- | --- | --- | --- | --- | --- | --- |
| [Olegu621/xnu-peridot-ios](https://github.com/Olegu621/xnu-peridot-ios) | us | POCO F6 / Redmi Turbo 3, SM8635 | Bare-metal shim, `fastboot boot` | 16.7.16 (iPhone X `d22ap`) | README: APSL 2.0 for the kernel patches; no LICENSE file in the tree | Kernel boots, iOS userland runs, lock screen confirmed on device (`README.md`, `NOTES.md`) |
| [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) | dipa-studio | Redmi Note 10 Pro, SM7150 | Bare-metal shim, `fastboot boot` | 16.7.16 (iPhone X) | MIT (`LICENSE`, 1068 B) | Home screen reached 2026-10-18 (README); 907 stars |
| [hoshikv/xnu-darwin-peridot](https://github.com/hoshikv/xnu-darwin-peridot) | hoshikv | POCO F6, SM8635 | Bare-metal shim **and** Mu-Silicium UEFI + `XnuLoader` EFI app | 16-era, xnu-8796 | No LICENSE file; `mu/XnuLoader/XnuLoader.inf` says BSD-2-Clause-Patent | 131-patch kernel series; latest commits port the Goodix BerlinD SPI transport and the PON Gen3 button bits. No README, no stated runtime state |
| [kvfsrl/UniXNU-peridot](https://github.com/kvfsrl/UniXNU-peridot) | kvfsrl | POCO F6 / Redmi Turbo 3, SM8635 | UEFI (Mu-Silicium) + UniXNU boot manager + DarwinBoot | 16.7.16 (iPhone10,3) target declared in `board.toml` | No LICENSE file | "device repository (WIP)". 1 commit ("Add peridot device repo skeleton"); board file + ADT scripts only, no drivers, no touch driver |
| [kvfsrl/peridot-xnu-build](https://github.com/kvfsrl/peridot-xnu-build) | kvfsrl | Builds the peridot XNU | CI only, no source | xnu-8796, `MACHINE_CONFIG=PERIDOT` | No LICENSE file | 3 commits; the only file is `.github/workflows/xnu.yml`, which clones `hoshikv/xnu-darwin-peridot` and builds it with `blacktop/darwin-xnu-build` |
| [hoshikv/UniXNU-peridot](https://github.com/hoshikv/UniXNU-peridot) | hoshikv | POCO F6, SM8635 | UEFI + UniXNU | as `board.toml` | MIT (inherited from UniXNU) | A **fork of `UniXNU/UniXNU`** with `board.toml` and `configs/peridot_defconfig` added; latest commit "Add peridot device repository files (WIP)" |
| [hoshikv/UniXNU-peridot-standalone](https://github.com/hoshikv/UniXNU-peridot-standalone) | hoshikv | POCO F6, SM8635 | UEFI + UniXNU | as `board.toml` | No LICENSE file | README: "State: device repository, WIP." Same files as `kvfsrl/UniXNU-peridot` without the build output |
| [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) + [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) + [uefi-sheng](https://github.com/UniXNU/uefi-sheng) | UniXNU org | Xiaomi Pad 6S Pro, SM8550 | UEFI (Mu-Silicium) + UniXNU boot manager + DarwinBoot loader | iPadOS 17.7.11 (`j120ap`) | BSD-2-Clause-Patent | The reference port: lock screen and home screen reached, native UFS root, touch works, native Metal GPU compositing works (`docs/devices/sheng.md`) |
| [2713385115/UniXNU-nx729j](https://github.com/2713385115/UniXNU-nx729j) | 2713385115 | Nubia RedMagic 8 Pro/8 Pro+, SM8550 | UEFI (Mu-Silicium) | iPadOS 17.7.11 | BSD-2-Clause-Patent (`LICENSE`) | "Phase A" only: their own UEFI reaches BDS on the device and stops at "Failed to Find an Operating System"; no iOS kernel yet (`STATUS-适配进度.md`). UniXNU's own device page says "runtime state not documented" |
| [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) (+ `hackios-xnu-userland`) | OOSEX GANG | POCO X5 Pro 5G (`redwood`), Snapdragon 778G / SM7325 | UEFI (Mu-Silicium) + `xnuloader` EFI app | 16.5 (xnu-8796.121.2) | APSL 2.0 (`LICENSE`, 19829 B) | The most complete public port: lock screen, home screen, apps, Mesa/freedreno GPU, Wi-Fi, sound, camera (README, with photos) |
| [UniXNU/NativeGeniSpi](https://github.com/UniXNU/NativeGeniSpi) | UniXNU org | GENI SPI controller (module) | n/a | n/a | BSD-2-Clause-Patent | "Current state: built into the kernel" (README) |

## 2. The three peridot-specific repositories

### 2.1 `kvfsrl/UniXNU-peridot` (local clone `/tmp/opencode/unixnu-peridot`)

A UniXNU *device repository*, not a kernel port. Complete file list:

| File | What it is |
| --- | --- |
| `board.toml` | `soc = "SM8635"`, IPSW `iPhone10,3` 16.7.16 `20H392`, `chip_id = 0x8015`, kernel archives, firmware (`Platforms/Xiaomi/peridotPkg/peridot.dsc`), payload volume `PERIDOTES` (192 MiB), `dram_size = 0x380000000`, kernel args, ADT options, and a `/reserved-memory/splash_region` node for the stock ABL |
| `scripts/make-peridot-adt.py` | 306 lines. Generates the Apple device tree: GIC distributor `0x17100000`, redistributor `0x17180000`, 19.2 MHz timebase, 8 cores (`0x000`–`0x700`: 3x A520, 4x A720, 1x X4), UFS host `0x1d84000`, DRAM base `0x80000000` size `0x380000000` |
| `scripts/extract-d22ap-identity.py` | 136 lines. Extracts the real iPhone X identity from `DeviceTree.d22ap.im4p` of the restore image, so MobileGestalt reads the genuine product strings |
| `scripts/d22ap_identity.py` | 125 lines. Generated module holding that identity (root strings, `/product`, `/buttons`, `/defaults`, `/options`) |
| `scripts/build-arm-driver.py` | 109 lines. Compiles a driver against the real source-kernel ABI read from `IOPlatformExpert.cpo.json`; supports `NativeUfs`, `NativeAdreno`, `NativeHidInput`, `NativeLegacyDisplay`, `NativeMobileDisplay`, `NativeOfflineGPU` and others |
| `scripts/patch-xnu-entry-probe.py`, `scripts/xnu-panic-screen.c` | 426 lines + a freestanding C file. Injects the XNU entry probe and the panic screen into unused kernel padding; the renderer draws on the kernel's own framebuffer mapping |
| `build/peridot/boot/kernel.macho.adt` | 19140 bytes. A generated ADT: `iPhone10,3` identity with `xiaomi,peridot` / `qcom,sm8635` appended to `compatible`, `/chosen` with `random-seed`, `unique-chip-id`, `dram-base`, `dram-size`, `nvram-proxy-data`, `/memory-map`, `/cpus` with `arm,cortex-a520` and the 19.2 MHz timebase |

State, by its own files: one commit, "Add peridot device repo skeleton". There is no
README, no STATUS file, no `drivers/` directory and no touch driver; the GitHub
description says "(WIP)". Nothing in the tree claims the kernel boots or that a lock
screen was reached. Reusable: `scripts/make-peridot-adt.py` and `scripts/d22ap_identity.py`
(the D22AP identity extraction is the piece we would otherwise redo);
`scripts/build-arm-driver.py` (ABI-correct driver compilation against a real
`IOPlatformExpert.cpo.json`); and the `[board]` / `[payload]` / `[payload.adt]` /
`[[bootimg.dtb_node]]` structure of `board.toml`, the UniXNU-native equivalent of our
`tools/mkadt.py`.

### 2.2 `hoshikv/xnu-darwin-peridot` (local clone `/tmp/opencode/xnu-darwin-peridot`)

A kernel patch series plus the bare-metal shim and host tools, the same shape as
this repository:

| Path | What it is |
| --- | --- |
| `xnu/patches/0001..0131` | 131 patches on xnu-8796. The 0001–0120 series is our sweet series renamed `sweet` → `peridot`; 0121 `peridot-touch-geni-spi`, 0125 `peridot-touch-fifo-fallback`, 0130 `peridot-goodix-berlind-spi` and 0131 `peridot-pon-gen3-button-bits` are new |
| `shim/` | `start.S`, `shim.c`, `mmu.c`, `ufs.c`, `loader.c`, `console.c`, `fdt.c`, `font.h`, `shim.ld`, `build.sh` — the same bare-metal loader design as ours, but with `IMEM_RESTART_REASON = 0x1468065c` (`msm-imem@14680000 + restart_reason@65c`) where ours uses scratch at `0xa9000000`, because peridot runs under Gunyah |
| `mu/XnuLoader/XnuLoader.c` (1279 lines), `mu/XnuLoader/XnuLoader.inf`, `mu/install.sh` | A UEFI application for peridot that validates the `XNUKERN1` / `XNUFDT01` / `XNUROOT1` tail chain published by BootShim at `0xA7400000`, copies the kernel to a 32 MiB-aligned base, applies the `LC_DYLD_INFO` rebase slide, rewrites `/chosen/dram-base`/`dram-size`, builds `boot_args` and jumps to EL1 with the MMU off |
| `tools/` | `mkpayload.py`, `mkadt.py`, `adt.py`, `mkinit.py`, `mkiosroot.sh`, `peridot-init.S`, `peridot-log.py`, `peridot-crash.py`, `peridot-root-tweaks.py`, `patch-launchd-plist.py`, `qemu-peridot.py`, `xnu-cc.py`, `check_payload.py`, `hfsroot/main.go` |

State, by its own files: one squashed commit, "xnu: use Gen3 PON status bits for
power and resin keys". `.gitignore` excludes `*.md`, so there is no README and no
status file. The pre-squash history (GitHub API) shows the work sequence, ending
with "touch: port Goodix BerlinD SPI for Peridot" and the panel scaling to
1220x2712 (2026-10-22). No public statement says the kernel boots or that
SpringBoard runs. Reusable: `mu/XnuLoader/XnuLoader.c` and `mu/install.sh` if we
ever want a UEFI-bootable peridot image; patches 0121/0125/0130 (Goodix BerlinD
SPI framing: `GOODIX_SPI_READ 0xf1`, `GOODIX_SPI_WRITE 0xf0`,
`GOODIX_FW_VERSION_ADDR 0x10014`, `GOODIX_IC_INFO_ADDR 0x10070`, plus the
FIFO-if-disabled fallback) and 0131 (PON Gen3 status bits `KPDPWR_N_SET 0x80`,
`RESIN_N_SET 0x40`).

### 2.3 `kvfsrl/peridot-xnu-build` — no local clone

The directory `/tmp/opencode/peridot-xnu-build` does not exist on this machine, so
nothing was read from disk. Read over the GitHub API instead: 3 commits (`ci: build the
PERIDOT machine config`, `ci: rebuild peridot XNU with patch 0083 (peridot USB)`, `ci:
build peridot XNU from hoshikv/xnu-darwin-peridot`) and exactly one file,
`.github/workflows/xnu.yml` (4931 bytes) — no source, no board data, no drivers. The
workflow clones `https://github.com/hoshikv/xnu-darwin-peridot`, copies
`xnu/patches/*.patch` into `blacktop/darwin-xnu-build` at commit
`663906528d1cd5ea8fde7de6f16de73db9511549`, forces `ARCHS=... arm64` and builds
`MACHINE_CONFIG=PERIDOT` on `macos-14`. Reusable: nothing but the CI recipe; our
`.github/workflows/xnu.yml` already does the same with `MACHINE_CONFIG=SWEET` and pins
the same darwin-xnu-build commit.

## 3. What is already solved elsewhere

| Problem | Where it is solved | Exact file |
| --- | --- | --- |
| GENI SPI engine, FIFO (PIO) and DMA transports | `UniXNU/NativeGeniSpi` | `GeniSpi.c` (`handle_tx`/`handle_rx` FIFO path; `transfer_dma` with the `SE_DMA_{TX,RX}_{PTR,ATTR,LEN,IRQ}` sequence and `GENI_DMA_AHB_ERR` handling), `GeniSpi.h` (`GENI_IF_DISABLE_RO 0x64`, `GENI_FW_REVISION_RO 0x68`, `SE_SPI_*`), `Test/geni-spi.c` |
| Goodix BerlinD protocol | Mainline Linux (Neil Armstrong, 2024-01-30) | `drivers/input/touchscreen/goodix_berlin_core.c`, `goodix_berlin_spi.c` (git.kernel.org log for that file) |
| Built-in XNU touch-driver skeleton | `UniXNU/NativeNovatekTouch` | `NvtTouchCore.c` (pure processing), `NvtProtocol.c`/`.h` (protocol over a `struct nvt_bus`); board side in `UniXNU-sheng/package/TouchBoard.h`, `TouchProbe.cpp` |
| UEFI + loader boot chain | `UniXNU/UniXNU`, `UniXNU/uefi-sheng` | `drivers/DarwinBoot` (`DarwinBootPkg/Driver/DarwinBoot.c`, `BootArgs.h`), `package/UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.c`, `package/UniXNUPkg/Library/UniXNULib/{Boot,Slice,Toml}.c`, build driver `scripts/unixnu/{payload,userdata}.py`. Peridot-specific: `hoshikv/xnu-darwin-peridot` `mu/XnuLoader/XnuLoader.c`, `mu/install.sh`. A second independent implementation: `oosexgang/hackios-xnu-kernel` `uefi/mu-silicium.patch` (BootShim copies the `XNUP` container to `PAYLOAD_BASE`; `BootManagerLib` starts the loader instead of the menu), `uefi/files/Platforms/Xiaomi/redwoodPkg`, `xnuloader/loader.c`, `xnuloader/mkpayload.py`, `mkxnuimg.py` |
| ADT / device-tree generation | `UniXNU/UniXNU`, `UniXNU-sheng` | `docs/porting/device-tree.md` (Apple node encoding, and the `/chosen`, `/cpus`, `/arm-io`, `/defaults`, `/options` contract), `scripts/qemu-entry/make-dt.py`, `UniXNU-sheng/scripts/make-sheng-adt.py`. Peridot-specific: `kvfsrl/UniXNU-peridot` `scripts/make-peridot-adt.py`, `scripts/extract-d22ap-identity.py`, `scripts/d22ap_identity.py` |
| Display and GPU | `UniXNU/NativeMobileDisplay`, `UniXNU/NativeMetal`, `UniXNU/NativeAdreno` | `NativeMobileDisplay.cpp`, `NativeLegacyDisplay.cpp`, `NativeGOPPresenter.cpp`, `OriginalMobileFramebufferABI.h`; `NativeMetal` (Metal frontend, metal2vulkan), `kits/AirNir` (AIR to NIR to IR3), `NativeAdreno` (Adreno 740: `NativeAdrenoCpInit.cpp`, `NativeAdrenoGmuService.cpp`), `kits/AdrenoPM4`, `kits/AdrenoFirmware`. A second implementation: `oosexgang/hackios-xnu-kernel` `xnu/files/bsd/dev/redwood_adreno.c` (4125 lines), `xnu/files/iokit/Kernel/MiniIOSurface.cpp`, `MiniIOMobileFramebuffer.cpp`, `MiniAppleKeyStore.cpp` |
| HID digitizer publication | `UniXNU/NativeHidInput` | `NativeHidInput.cpp` |
| iOS userland image pipeline | `UniXNU/UniXNU` | `scripts/unixnu/userdata.py` and `docs/guide/userdata.md` (the `SHNGROOT` manifest, `OS` and `Cryptex1,SystemOS` windows, root selection by UUID), `scripts/seed-data-grow.py`. A second implementation: `oosexgang/hackios-xnu-userland` (the APFS image builder and `ios165/fetch.sh`, referenced from the kernel repo's README) |
| SPI bit-bang over the touch pins when the engine cannot be used | `oosexgang/hackios-xnu-kernel` | `xnu/files/bsd/dev/redwood_goodix.c` (1515 lines): Goodix GT9916 over the serial engine in FIFO mode, or the same four pins driven by hand; its register layouts are taken from Linux `goodix_berlin_*` and `focaltech_core` |

## 4. What only we have

- The **SWEET machine configuration** for xnu-8796 (`xnu/patches/0004-sweet-machine-config.patch`
  adds `SWEET` to `SUPPORTED_ARM64_MACHINE_CONFIGS` with `-march=armv8.2-a+crypto` and
  forces plain `arm64` instead of `arm64e`), and the whole 0001–0108 patch series built on
  it. Ours is `SWEET`; `hoshikv`'s is the same series renamed to `PERIDOT`.
- The **bare-metal shim**: `shim/start.S`, `shim.c`, `mmu.c`, `ufs.c`, `loader.c`,
  `console.c`, `fdt.c`, `shim.ld`, `build.sh`. It enables the MMU itself, reads the live
  FDT, pulls the kernel and the iOS root over UFS and hands over, with a freestanding
  panic screen. No UEFI, no Mu-Silicium, no separate loader binary.
- **iOS 16.7.16 userland running on SM8635**: `README.md` records launchd, the system
  daemons, backboardd and SpringBoard running; `NOTES.md` records the user confirming the
  lock screen ("экран блокировки видно") on the 0092 image.
- The **SPMI key driver** for peridot's v7 arbiter and PMK8550 PON
  (`xnu/patches/0095-sweet-spmi-peridot-probe.patch`, `0097-peridot-spmi-v7-keys.patch`):
  mixed v5/v7 register tables, PPID fix, 512/1024 bounds, a single poller owning the
  observer channels, read-only access, debounce. Hardware-verified: all three buttons
  produced matched down/up HID events.
- The **display path** presenting a **1125x2436** logical iPhone X surface on the
  1220x2712 panel (`xnu/patches/0091-peridot-logical-display-and-peripheral-guard.patch`,
  `0103-peridot-fb-force-on.patch`, `0105-sweet-frame-console-capture.patch`,
  `0107-sweet-frame-console-chunks.patch`). The 1125x2436 canvas is what removed the
  `AppleM2ScalerCSCDriver` loop; the panel is 1220x2712 with touch raw range
  0..19520 / 0..43392 (16x fixed point).
- The **SMMU work**: a read-only readiness profile (`0100-peridot-gpi-smmu-readonly-profile.patch`),
  a bounded private DMA path (`0101-peridot-gpi-spi-bounded-private-dma.patch`),
  scoped physical-context routing (`0102-peridot-smmu-physical-context-route.patch`)
  and a page-aligned arena (`0103-peridot-gpi-page-aligned-arena.patch`). The hardware
  finding is that the firmware replaces the requested S2CR `BYPASS` with `FAULT`, so an
  existing disabled CB (`CBAR.type1`, `SCTLR.M=0`) must be reused.
- The **persistent console log** and the **boot-cycle tooling**:
  `xnu/patches/0047-sweet-persistent-console-log.patch`, `0105`, `0106-peridot-console-pacing.patch`,
  `0107`; `tools/peridot-cycle.py` (BCB backup, temporary `fastboot boot`, timed self-reset,
  log pull), `tools/peridot-frame-from-log.py` (PNG screen capture from the kernel),
  `tools/sweet-log-pull.sh`, `tools/peridot-pstore-pull.sh`.

## 5. Open questions

Unresolved in the public state, not questions we intend to send:

- Whether the peridot UniXNU device repository has a working touch driver. It has no
  `drivers/` directory at all; `kvfsrl/UniXNU-peridot` has no README or status file,
  and `hoshikv/UniXNU-peridot-standalone` says only "device repository, WIP".
- Whether the RedMagic UEFI boots on other SM85xx phones. `UniXNU-nx729j` proves it on
  the RedMagic 8 Pro+ itself, but `UniXNU`'s device page lists only sheng as a supported
  device and describes nx729j as an unmaintained community port.
- Whether the serial-engine DMA path works with the FIFO interface disabled.
  `UniXNU/NativeGeniSpi` implements both paths, but no public page states that the DMA
  path was exercised where `GENI_IF_DISABLE_RO` bit 0 is set.
- Whether `hoshikv/xnu-darwin-peridot` ever reached a running iOS userland. It has no
  README, no status file and a squashed history, and the CI repository only builds
  kernels.
- Whether the peridot UEFI package (`Platforms/Xiaomi/peridotPkg`) is public. No
  `uefi-peridot` repository exists; only the device repository and the loader are
  published.
- Whether `dipa-studio/ios-sweet`, the upstream of this repository, is still maintained:
  its README dates the sweet port to 2026-10-18, one branch, `main`. And whether
  UniXNU's peridot work will be folded into the organisation: there is no peridot entry
  in `unixnu.com/devices/` or in `docs/devices/index.md`.

## Sources

Local: the clones listed above, plus this repository's `README.md`, `AGENTS.md`,
`NOTES.md`, `shim/`, `tools/`, `xnu/patches/` and `.github/workflows/xnu.yml`.

Remote, all fetched 2026-10-22: every repository linked in the table above
(`github.com/kvfsrl/…`, `github.com/hoshikv/…`, `github.com/UniXNU/…`,
`github.com/2713385115/UniXNU-nx729j`, `github.com/oosexgang/hackios-xnu-kernel`,
`github.com/oosexgang/hackios-xnu-userland`, `github.com/dipa-studio/ios-sweet`), plus
`unixnu.com/devices/` and the mainline Linux log for the Goodix Berlin driver,
`git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/log/drivers/input/touchscreen/goodix_berlin_core.c`.
