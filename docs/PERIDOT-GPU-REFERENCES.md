# External GPU work relevant to peridot

Reviewed 2026-10-23. These sources inform separate parts of the port; they
do not establish that our iOS hardware acceleration works.

## AdrenoToolsDrivers 24.3.0 R2

[Release](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v24.3.0_r2)
contains an Android AArch64 Vulkan library. Its linked Mesa changes enable
FD735 and remove assumptions that the host uses 4 KiB pages. Inspected Mesa
25.0.7 already contains both changes. The Android library depends on Bionic
and Android services, so its binary cannot be loaded as a Darwin driver.

## Termux discussion 23914

[Discussion](https://github.com/termux/termux-packages/discussions/23914):
the author reports working Adreno735 acceleration on a Xiaomi Pad7 Pro after
an update to Mesa25.0.1. There is no accepted answer or XNU kernel backend.
The reported frame rates are the author's observations, not our measurements.

The [linked xMeM commit](https://github.com/xMeM/termux-packages/commit/589bd79af48d99329c717384b59490352762cc36)
updates Mesa and adjusts EGL/Zink integration with Qualcomm's proprietary
Vulkan driver. This still relies on the existing Android GPU kernel stack.
Its EGL device-discovery `software=true` flag is not by itself evidence of
CPU rendering: Zink can still execute through a hardware Vulkan driver.

## UniXNU-pong

[Pinned source](https://github.com/Chengdong-Yu/UniXNU-pong/tree/51911de5fb01dfc54bd32c1e85fc70c482ff0497)
targets Nothing Phone2 / SM8475 / Adreno730v3 and iOS16.6.1. Its README
reports UI/touch working and labels GPU/Metal execution unconfirmed.
The Metal documentation distinguishes compiling/linking the libraries from
discovering a Metal device and completing real GPU commands.

Useful references include the iOS16 private Metal ABI analysis and PAS13
metadata/authentication sequence. Its A730 geometry, three active HFI queues,
fixed carveout and CPU4K policy are not peridot's A735/HWSCHED17/CPU16K setup.

## EAGL and Gallium

[ios-sailfish sweetgl](https://github.com/joranv1/ios-sailfish/tree/d7a74344828c87d37c64f87f237525cb65b7e5f5/tools/sweetgl)
provides an iOS16 EAGL/Gallium frontend and a DRM-shaped IOKit transport.
It requires real BO allocation, task mappings, IOSurface import, command
submission and completed fences from a kernel service. It is not a complete
peridot driver: its GPU ID is hard-coded to530, and private QuartzCore hooks
are tied to its particular shared cache and must be checked against ours.

The official [Mesa25.0.7 source archive](https://archive.mesa3d.org/mesa-25.0.7.tar.xz)
was inspected. `freedreno_screen.c` dispatches generation7 to `fd6_screen_init`;
`fd6_emit.cc` includes actual A7XX paths, and `freedreno_devices.py` includes
FD735. Thus the candidate GLES route has source support beyond Turnip alone.
This is source evidence, not a successful iOS cross-build or rendered frame.
