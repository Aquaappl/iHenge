# M2 — reference qemu-vphone on the Mac mini

qemu-vphone (branch `vphone`, commit `74a7cda`) built on macOS for the reference boot logs in
`runs/ref/`. TCG only, no GPU, no vsock. vphone-linux's `build.sh` targets Linux (reims-vgpu,
Vulkan), so this is a separate recipe.

## Build

```bash
brew install ninja meson libtasn1 nettle lzfse libslirp
git clone --depth 1 -b vphone https://github.com/YahiaAngelo/qemu-vphone.git ~/refs/qemu-vphone-build
cd ~/refs/qemu-vphone-build && git submodule update --init --depth 1 util/mlib
mkdir build && cd build
../configure --target-list=aarch64-softmmu --enable-slirp --enable-lzfse --enable-nettle --enable-zstd \
  --disable-hvf --disable-werror --disable-qom-cast-debug \
  --extra-cflags=-I/opt/homebrew/include --extra-ldflags=-L/opt/homebrew/lib
ninja qemu-system-aarch64
```

- `--disable-hvf`: with HVF on, the stock `vmapple` machine is enabled and selects `MAC_PVG_MMIO`,
  which needs the reims-vgpu library (a Kconfig contradiction without it). The reference runs on TCG
  like on Linux anyway.
- `--disable-pvg` does not work for the same reason; leaving PVG on auto is fine (not found).
- The Homebrew include/lib paths are needed because lzfse ships no pkg-config file.

## Local portability edits (`tools/qemu-vphone-macos.patch`)

Apply after cloning: `git -C ~/refs/qemu-vphone-build apply ~/iHenge/tools/qemu-vphone-macos.patch`.

Two Linux-only spots fail to compile on macOS; both are debug/measurement code:

1. `hw/usb/dev-hid.c`, the `VPHONE_TOUCH_TRACE` timestamp: `CLOCK_BOOTTIME` → `CLOCK_MONOTONIC`
   under `#ifdef __APPLE__`.
2. `target/arm/helper.c`, the `VPHONE_MARK` perf counters: guard `<linux/perf_event.h>` /
   `<sys/syscall.h>` with `#ifdef __linux__`, define `PERF_COUNT_HW_CPU_CYCLES 0` and
   `PERF_COUNT_HW_INSTRUCTIONS 1` otherwise, and make `vphone_mark_open()` return `-1`
   (`errno = ENOSYS`) off Linux. Only matters with `VPHONE_MARK=1`.


## Run

The reference boots an APFS clone of the vphone-cli VM so it never touches the original:

```bash
mkdir -p ~/vphone-vms/iphone16pro-ref
cd ~/.vphone/machines/iphone16pro
for f in Disk.img nvram.bin SEPStorage OpenAVB.bin OpenSEP.bin config.plist; do cp -c "$f" ~/vphone-vms/iphone16pro-ref/; done
python3 -c 'import plistlib,sys; d=plistlib.load(open(sys.argv[1],"rb")); print(hex(plistlib.loads(d["machineIdentifier"])["ECID"]))' \
  ~/vphone-vms/iphone16pro-ref/config.plist > ~/vphone-vms/iphone16pro-ref/ecid
cd ~/refs/vphone-linux
export VPHONE_RAS=0 VPHONE_RET_PREFETCH=0 VPHONE_XPAGE_LINKS=0 VPHONE_INLINE_LOOKUP=0 VPHONE_IBC=0 \
  QEMU_TLB_UNSYNCED=0 VPHONE_TARGET_PAGE_BITS=12 VPHONE_TLB_RANGE_PAGES=0
QEMU=~/refs/qemu-vphone-build/build/qemu-system-aarch64 SMP=4 MEM=4G CID=0 \
  ./run-vphone.sh ~/vphone-vms/iphone16pro-ref -monitor unix:/tmp/vref-mon.sock,server,nowait \
  < /dev/null > ~/iHenge/runs/ref/serial.log 2>&1
# stop it with: pkill -f "build/qemu-system-aarch64 -M"
```

- **The `VPHONE_*` fast paths must be off on macOS.** With run-vphone.sh's defaults, nothing ever
  reaches the UART: the AP and SEP vCPU threads spin at 100% inside `helper_vphone_ras_miss`
  (`accel/tcg/cpu-exec.c`), the guest makes no progress, and the monitor's `stop` hangs in
  `pause_all_vcpus` waiting for them (seen with `sample <pid>`). With all of them off, OpenAVB prints
  `OpenBootROM:` at once. Which one is at fault was not bisected; `VPHONE_RAS` is the prime suspect.
- With MTTCG, `info registers` on a running vCPU shows a stale PC (it is only written back when the
  vCPU leaves its chained blocks). Use `sample` on the host, or `stop` first.
- `ecid` must be `0x`-prefixed hex (QEMU rejects bare hex: "Parameter 'ecid' expects integer").
- `CID=0`: vhost-vsock is Linux-only; vsock is not needed to boot.
- macOS has no `timeout`, and QEMU ignores a perl `alarm`; kill it with `pkill`.
- Don't boot the clone while vphone-cli runs the original (separate files, but same identity).

## Result (2026-11-25): `runs/ref/ref-boot-gfx0.log`

Not in git (`runs/` is ignored: it holds the machine's ECID). From a cold start, in about 3 minutes:

1. `OpenBootROM:` — SEP bootstrap over the mailbox (`SEP RX ready/empty`), `SEP bootstrap complete`,
   `iLLB container accepted`, `virtual firmware prepared`
2. LLB / iBoot: `Microkernel iBoot for vresearch101`, `Board 0x90 (vresearch101ap)`,
   `iBoot-13822.100.791.502.1`
3. XNU: AppleImage4, AMFI (`Booted in a VM`), `BSD root: disk0s1`, APFS mountroot, TXM, SEP manager
4. launchd (`hello from launchd.1`), the boot tasks, data protection, USB touchscreen enumerated
5. `rebooting due to critical process crashes: SpringBoard` → `ApplePSCI - system reset`.
   Expected with no GPU attached; this run is the reference up to launchd (M3–M6). M7 needs a GPU.
