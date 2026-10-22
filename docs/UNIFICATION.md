# Unification plan

## Goal

One repository that boots iOS 16 on `peridot` (POCO F6, SM8635), assembled from the
strongest part of each of the projects below instead of re-deriving them. Each project
already solved one of the problems we are solving; the plan is to say, for every file we
touch, which upstream project it came from, what we take from it, and what we still have
to write ourselves.

The port's own state is authoritative in `NOTES.md`. This document does not repeat the
hardware facts; it only says where a fact is going to be implemented and out of which
tree it will be read.

Nothing here changes the kernel patch discipline: `xnu/patches/` stays the single source
of truth, and every stage below ends in a numbered patch built by CI and booted on the
phone.

## Kernel-side projects

| Project | What it is good at | What we adopt | Where it lands in our tree |
| --- | --- | --- | --- |
| `oosexgang/hackios-xnu-kernel` (POCO X5 Pro / redwood) | A Goodix BerlinD panel driven over the serial engine, and the same panel driven by hand when the engine cannot be used. No SPI, pinctrl, clock or HID framework: the driver does all of it. | `redwood_goodix.c`: `bitbang_init()` and `bitbang_transfer()` (mode 0, MSB first, chip selected for the whole frame), the frame builders `goodix_read()` (`0xf1` + BE32 address + three `0xff` dummy bytes + payload) and `goodix_write()` (`0xf0` + BE32 address + data), and `goodix_checksum_ok()` (the 16-bit sum of the block's bytes sits in its last two bytes; an all-zero or all-`0xff` block is no answer at all). Also `gpio_configure()`/`gpio_set()`/`gpio_get()`/`gpio_output()` for TLMM: `GPIO_CFG(n) = tlmm + 0x1000*n` with pull `[1:0]`, function `[5:2]`, drive `[8:6]`, output-enable `[9]`, and `GPIO_IO(n) = +4` (bit 0 in, bit 1 out). | The transport is already ported in `xnu/patches/0108-peridot-bitbang-spi-and-touch-power.patch` as `bsd/dev/sweet_spi_peridot.c`; the framing and checksum layer is reused verbatim by the touch poller that 0108 deliberately leaves out. The TLMM helpers are the model for the pin writes in `bsd/dev/sweet_rpmh_peridot.c` (reset on GPIO128) and for the panel pins 16..19. |
| same | The serial engine in FIFO mode, used where the engine allows it | `geni_spi_init()` (FIFO geometry, packing configuration, watermarks, `SE_IRQ_EN |= 0xf`), `geni_transfer()`, `geni_drain()`, `geni_recover()`, and the refusal when `GENI_IF_DISABLE_RO` says the FIFO interface is off | Already reimplemented in `bsd/dev/sweet_spi_peridot.c`, where `peridotspi=1` is the FIFO and PIO read-only probe. It stays as the documented reference for the FIFO fallback that peridot cannot use; it is not the transport. |
| same | Touch samples reaching IOHID without a HID stack | `redwood_input.c`: `/dev/redwood-touch`, a 512-entry ring of samples, `sys/redwood_input.h`, and the latency hook `redwood_tl()` that writes into the loader's log | Our `bsd/dev/sweet_touch.c` and `bsd/dev/sweet_keys.c`, and the digitizer path under `iokit/Kernel/arm/hid/` that the touch candidate already patches |
| same | A complete IOKit platform without Apple's drivers | `MiniIOMobileFramebuffer.cpp` (classes `IOMobileFramebuffer`, `IOMobileFramebufferUserClient`, `IOAcceleratorES`), `MiniIOSurface.cpp`, `MiniIONetworkStack.cpp` (`IONetworkStack` plus its user client), `RedwoodGPU.cpp` (a DRM-shaped user client over freedreno) | Read against our `iokit/Kernel/arm/SweetFramebuffer.cpp` and `iokit/Kernel/arm/SweetSurface.cpp`, which already work; the network stack is the model for a future `iokit/Kernel/arm/SweetNetworkStack.cpp`. The Adreno work in `redwood_adreno.c` is redwood-specific and is not imported. |
| `UniXNU/NativeGeniSpi` | A portable GENI/QUPv3 SPI controller: no board addresses, clocks or pins, the exact register map, FIFO mode and the serial engine's own DMA mode | `GeniSpi.c`/`GeniSpi.h`: the register map (`GENI_IF_DISABLE_RO 0x64`, `GENI_FW_REVISION_RO 0x68`, `SE_GENI_DMA_MODE_EN 0x258`, `SE_GENI_M_CMD0 0x600`, `SE_GENI_M_IRQ_STATUS 0x610`, `SE_GENI_TX_FIFOn 0x700`, `SE_GENI_RX_FIFO_STATUS 0x804`, `SE_IRQ_EN 0xe1c`, `SE_HW_PARAM_0 0xe24`, `SE_DMA_GENERAL_CFG 0xe30`), `transfer_dma()` (the engine's DMA master reads and writes the caller's buffers, with the caller owning the translation and the cache), `struct geni_io` with `rd`/`wr`/`delay_us` callbacks, `struct geni_spi.dma` with `clean`/`clean_inval`/`inval` and `tx_phys`/`rx_phys`, and the `allow_fifo_disabled` flag | Ported into `bsd/dev/sweet_geni_spi.c` as the transport behind `bsd/dev/sweet_spi_peridot.c`. The `geni_io` callback shape is exactly the seam our GPI and SMMU code already has, so the port is an adapter and not a rewrite. |
| `2713385115/UniXNU-nx729j` (RedMagic 8 Pro, SM8550) | A device repository: board description, ADT generation, preflight, and a written specification for the one driver that did not exist yet | `board.toml` as the schema; `scripts/make-nx729j-adt.py` (ADT generation); `scripts/collect-nx729j.py` (read-only GPT and `userdata` LBA geometry, over `adb` or from a dump); `scripts/check-nx729j.py` (preflight: hard failures stop the build, unbrought-up parts are warnings); `scripts/build-session-image.sh` and `scripts/build-flash-image.sh` (tagged images, size checked against the partition, never flashed); `tools/nx729j.py` (`doctor`, `measure`, `dump`, `boot`, `probe`) | The peridot device profile under `docs/peridot-profile.md` and `tools/peridot-doctor.py`, `tools/peridot-measure.py`, `tools/peridot-session-image.py`. Our own `tools/mkadt.py` stays the ADT generator: it already knows the peridot memory map. |
| same | A touch driver specification rather than code | `drivers/NativeGoodixTouch/README.md`: the staged acceptance (bus answers, then reset and interrupt, then configuration, then one point, then multi-point and gestures) and the 16-times fixed-point coordinate scaling | The acceptance criteria for our touch poller, written up as `docs/touch.md` and enforced by the staged verification in this document |
| `hoshikv/xnu-darwin-peridot` | Our own earlier lineage of this port | The patch series `xnu/patches/0001` to `0131`, notably `0063-peridot-goodix-geni-packing.patch`, `0121-peridot-touch-geni-spi.patch`, `0125-peridot-touch-fifo-fallback.patch`, `0130-peridot-goodix-berlind-spi.patch`; the bare-metal `shim/`; the UEFI driver `mu/XnuLoader/XnuLoader.c` | Mined for the Goodix framing work that 0108 does not contain, and as the model for a UEFI front end in stage 4. Its `tools/` are the ancestry of our `tools/`; nothing is copied wholesale. |

## Build and tooling projects

| Project | What it is good at | What we adopt | Where it lands in our tree |
| --- | --- | --- | --- |
| `UniXNU/UniXNU` | The port infrastructure: a build driver, a device-repository layout, a disciplined patch application, a UEFI boot manager and a documented boot chain | The `unixnu` driver's shape and its subcommands (`defconfig`, `fetch`, `tree`, `build`, `bootimg`, `ipsw`, `status`, `clean`); `scripts/unixnu/*.py` as a readable model for our host tools; the patch application discipline in `scripts/build-xnu-4k-baseline.sh` (reverse-check first, then apply, in name order, so a patch that is already applied is not applied twice) | The CI workflow `.github/workflows/xnu.yml` and `tools/validate-repo.py`. Our series stays in `xnu/patches/` with the same ordering rule. |
| same | The 4 KiB page series and the SMMU context audit | `patches/xnu/native-4k-bootstrap-capacity.patch`, `native-4k-signature-hash-cache.patch`, `native-4k-user-abi.patch`, `non-apple-pmap-field.patch`, `non-apple-iommu-bootstrap.patch`; `kits/ArmSmmu4K`; `scripts/check-arm-smmu-4k.py`, `check-smmu4k-*.py`, `check-native-4k-policy.py` | Reference only. peridot runs 16 KiB pages, so the patches must not be applied. What transfers is the method: recompute the SMMU context geometry for 16 KiB pages and audit the context bank by bank before touching it. |
| same | A UEFI boot manager and a four-step boot chain | `package/UniXNUPkg/Application/UniXNU` and `Driver/UniXNUDxe`, the `drivers/DarwinBoot` submodule, and `docs/concepts/boot-chain.md` (bootloader to BootShim, UEFI to UniXNU, UniXNU to DarwinBoot, DarwinBoot to XNU) | The model for stage 4, described in `docs/boot-chain.md`. |
| `kvfsrl/UniXNU-peridot` | A board profile for our own device | `board.toml` with `[board]`, `[ipsw]`, `[kernel]`, `[kernel.prelink]`, `[firmware]`, `[firmware.defines]`, `[payload]`, `[payload.adt]` and `[[bootimg.dtb_node]]`; `scripts/make-peridot-adt.py`; `scripts/d22ap_identity.py` and `scripts/extract-d22ap-identity.py` for the iPhone X identity | Cross-check against `tools/mkadt.py` and `tools/mkpayload.py`. The identity and memory-map values must agree with ours; where they differ, ours wins and the difference is recorded in `NOTES.md`. |
| `oosexgang/hackios-xnu-kernel` | The UEFI front end and the loader | `xnuloader/loader.c` with `efi.h` and `mkidentity.py`, the per-SoC `struct platform` in `common/devices/<codename>.platform.c` (`io_base`, `gic_dist`, `gic_redist`, `cores`, `core_shift`, `small_cores`, `psci_hvc`, `fb_base`), and `uefi/mu-silicium.patch`, which makes the boot shim copy the `XNUP` container to `PAYLOAD_BASE` and makes `PlatformBootManagerUnableToBoot()` call `XnuLoaderMain()` | The alternative front end in stage 4: `shim/uefi/` beside the existing bare-metal `shim/`. The patch is a porting recipe for a peridot Mu-Silicium package, not code to import. |
| same | A device profile and on-device tooling | `common/devices/*.conf` (the shell profile read by `common/build.sh` and `common/run.sh`: `NAME`, `KIND`, `SOC`, `UEFI_TARGET`, `DTB`, `FD_BASE`, `FD_SIZE`, `PAYLOAD_BASE`, `FB_BASE`, `BOOTARGS`, `FIRMWARE`), `common/new-device.sh`, and the `xl` verbs `enter`, `log`, `screenshot`, `recording`, `stop`, `reboot`, `state` | `docs/peridot-profile.md` plus `tools/peridot-profile.py`, and the `xl` verbs map onto our `tools/peridot-cycle.py` and `tools/peridot-frame-from-log.py`. |
| `kvfsrl/peridot-xnu-build` | A build repository for peridot | Nothing is imported: the checkout was not present in this environment and could not be read. It is treated as a CI automation reference only. | — |

## Staged adoption plan

### Stage 1 — Touch bus, read only

Unblocks the touch path.

- **Taken:** the bit-banged transport and the BerlinD framing from HackiOS; the panel rails
  and reset sequence already prepared in `bsd/dev/sweet_rpmh_peridot.c`.
- **From where:** `redwood_goodix.c` (`bitbang_transfer()`, `goodix_read()`,
  `goodix_checksum_ok()`, `goodix_probe()`) as adapted in
  `xnu/patches/0108-peridot-bitbang-spi-and-touch-power.patch`.
- **Written by us:** the peridot pin and clock setup (panel pins 16..19 as plain GPIOs,
  function 0, 6 mA, CS idling high), the mode-0 framing over four pins, the version read
  at `0x10014` and the `IC_INFO` read at `0x10070` with its 16-bit length prefix, and the
  bounded failure dump.
- **Verified on the phone:** `peridotspi=6` prints a `BERLIN` version string and a
  checksummed `IC_INFO`; a bad checksum or a missing chip prints the transport state and
  stops without touching the panel. The check is read-only: no reset pulse, no
  firmware or configuration write.

### Stage 2 — Touch events and HID delivery

- **Taken:** the sample-queue and HID structure from HackiOS (`redwood_input.c`), and the
  staged acceptance list from the UniXNU-nx729j specification.
- **From where:** `redwood_input.c` and `sys/redwood_input.h`; the four-stage acceptance
  list in `drivers/NativeGoodixTouch/README.md`.
- **Written by us:** the report poller on IRQ129, the frame decode (fixed 8-byte header,
  8-byte point stride, LE16 coordinates, checksummed, 0 to 10 contacts), the status-only
  `F0` acknowledgement of the event register, the coordinate transform from the panel's
  1220x2712 physical grid to the 1125x2436 logical surface with signed centering and
  clamping, and the release of contacts when the transport fails.
- **Verified on the phone:** one finger on the panel produces one HID digitizer event
  with a coordinate inside the screen; the lock screen answers a swipe; the frame
  capture (`sweetfbshot`) decodes to a PNG that shows the screen in the state the
  touch produced. A ten-finger test and an edge test follow.

### Stage 3 — Display, then GPU

- **Taken:** the IOKit display and surface structure from HackiOS, and the panel-timing
  extraction script from UniXNU-nx729j.
- **From where:** `MiniIOMobileFramebuffer.cpp` and `MiniIOSurface.cpp`, compared against
  our `iokit/Kernel/arm/SweetFramebuffer.cpp` and `SweetSurface.cpp`;
  `scripts/extract-panel-timings.py` for the panel description.
- **Written by us:** the peridot panel description and the scaler path that the current
  framebuffer copy does not need. The GPU is a separate, larger piece of work: the
  HackiOS driver is Adreno-specific for another SoC and is not a port candidate. It is
  planned as a stage of its own, after the display work settles, not as part of stage 3.
- **Verified on the phone:** the display keeps its present path and the 1125x2436
  surface; a screen capture taken from the kernel matches what the panel shows. For the
  GPU: a first frame drawn by the accelerator, then a CoreAnimation surface that is
  actually accelerated.

### Stage 4 — A second boot front end

- **Taken:** the UEFI front end and the loader from HackiOS, and the boot-chain contract
  from UniXNU.
- **From where:** `xnuloader/loader.c`, `uefi/mu-silicium.patch`, the per-SoC
  `struct platform` tables, and `docs/concepts/boot-chain.md`.
- **Written by us:** a peridot Mu-Silicium package (memory map, configuration map, the
  `XNUP` container at the right address), the peridot entry in the loader's platform
  table, and the payload volume layout.
- **Verified on the phone:** `fastboot boot` of the UEFI image reaches the loader's own
  screen and log, then the same kernel and the same iOS root that the bare-metal shim
  boots today. This stage adds an alternative; it replaces nothing until it is proven to
  boot the same session, and the bare-metal `shim/` keeps working in the meantime.

### Stage 5 — The port as a device profile

- **Taken:** the device-repository layout and the bring-up tooling from UniXNU and
  UniXNU-nx729j.
- **From where:** `board.toml` as the schema, `scripts/check-nx729j.py` as the preflight
  model, `scripts/collect-nx729j.py` as the read-only measurement model, and the
  `unixnu` driver's vocabulary for the commands.
- **Written by us:** `docs/peridot-profile.md` (the board facts with the source of each
  one), `tools/peridot-doctor.py` (host, adb, fastboot and image checks), and
  `tools/peridot-measure.py` (the `userdata` geometry, read-only).
- **Verified on the phone:** the doctor runs before every session and fails on a wrong
  image hash or a missing artifact; the measure script reproduces the partition geometry
  it wrote down.

## Rules that keep the port honest

1. **`xnu/patches/` is the single source of truth for kernel changes.** Every kernel
   change is a numbered patch in the series, applied in name order, and the series is
   what CI builds and the phone boots. Nothing that matters may live only in a local
   build tree; the ignored candidates under `research/` are experiments, not work.
2. **Foreign code lands in its own file, with its licence header intact.** A file
   imported from another project keeps that project's header, its copyright line and
   its SPDX identifier. It is not merged into a file of ours.
3. **A foreign licence is never mixed into the APSL XNU tree silently.** The kernel
   patches are APSL-2.0, like XNU itself. The HackiOS kernel drivers are APSL-2.0.
   `NativeGeniSpi`, UniXNU, UniXNU-nx729j and the Mu-Silicium UEFI content are
   BSD-2-Clause-Patent. The vendor Linux sources we read for the BerlinD protocol are
   GPL-2.0: constants and structure may be reimplemented, but their code is not copied
   into the tree. Any file that carries a licence other than APSL-2.0 says so in its
   first lines, and the file is listed in the licence notice of the repository.
4. **Per-board numbers are never copied between SoCs.** Clock votes, pinctrl registers,
   regulator addresses, stream IDs and GPIO numbers are re-derived from peridot's own
   device tree and register reads, and each adopted file carries a comment saying where
   its constants came from.
5. **Device dumps stay out of git.** `research/`, `dtb/`, `recovery/` and `backup/` are
   ignored; serial numbers, calibration data and the live device tree are read-only
   inputs, not repository content.
6. **Every claim is checked on the phone.** A ported function is not "done" until a boot
   log shows its expected output and the absence of a panic.

## Risks and open questions

- **The serial engine's FIFO interface is disabled on peridot.** The hardware reports
  `IF=1`, so the Linux-style FIFO path cannot receive on this board, and the engine
  refuses FIFO when `GENI_IF_DISABLE_RO` is set. `NativeGeniSpi`'s `allow_fifo_disabled`
  fallback applies only when no GPI channel exists; peridot has GPI channels, so porting
  `GeniSpi.c` does not by itself give us a working transport. It gives us a correct
  register map and a correct DMA sequence to run on GPI instead.
- **The GPI DMA path moves data but never completes.** The transfer produced the
  expected 69 bytes in the private receive buffer and the engine reported `rxlen=0x45`,
  but the event ring produced only `QUP_NOTIF` events and no transfer-complete event
  was ever accepted. Until that is explained, the DMA transport cannot be relied on and
  the bit-banged path is the one that must carry the touch.
- **The panels differ.** peridot's panel is a Goodix BerlinD (9916R) on SE4
  (`SPI@0xa90000`) with the panel pins on GPIO16..19, reset on GPIO128 and interrupt on
  GPIO129. HackiOS's panel is on GPIO52..55 with a different serial engine, a different
  TLMM base and a different reset and interrupt. Its pin numbers, its engine addresses
  and its timing constants do not transfer.
- **The SoCs differ.** HackiOS targets a Snapdragon 778G class part, UniXNU-nx729j an
  SM8550, UniXNU-sheng an SM8750, and we have an SM8635. Clock controller vote
  registers, pinctrl bank layouts, regulator names and RPMh command-database entries are
  per-SoC and per-board: HackiOS's LDO2 of the PM8350C and nx729j's `avdd`/`iovdd`
  names mean nothing here. Only protocol shapes and code structure transfer.
- **The 4 KiB page series does not apply.** We run 16 KiB pages. The SMMU context
  geometry we reuse (a disabled stage-2 context bank) was computed for another page
  size and has to be recomputed and documented for ours, and it is a borrowed resource:
  the note in `NOTES.md` about reusing CB0 must travel with the code.
- **The UnixNU boot chain is a different boot model.** It needs a UEFI firmware volume, a
  DarwinBoot loader inside the payload volume and a 192 MiB FAT volume, against our
  temporary `fastboot boot` of a bare-metal shim and an iOS root that lives in
  `userdata`. Stage 4 is therefore a second front end, not a replacement, and its memory
  map has to be rebuilt for peridot.
- **The panel configuration blob is still missing.** The firmware is present in the
  phone's Android and the panel reports without the configuration blob, but we have
  decided not to flash it. Whether the touch data is calibrated well enough for a
  usable UI without it is open.
- **The interrupt may not be usable.** HackiOS polls the interrupt line at 125 Hz rather
  than taking an interrupt, and we may have to do the same; the 8 ms poll in the touch
  candidate is the current answer and its cost in the idle path is not yet measured.
