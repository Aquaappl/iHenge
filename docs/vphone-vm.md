# M1 — the vphone-cli VM

Created on this Mac mini with vphone-cli (Launchpad), 2026-11-25. Lives outside this repo at
`~/.vphone/machines/iphone16pro` — never copy its contents into the repo or CI (see
"Apple のファームウェアをリポジトリにも CI にも入れない" in `CLAUDE.md`).

## VM folder contents

| File | What it is |
|---|---|
| `Disk.img` | Guest disk, 64 GB sparse |
| `nvram.bin` | Virtualization.framework aux storage (16 KiB header + NVRAM) |
| `SEPStorage` | SEP's persistent storage, 512 KiB |
| `OpenAVB.bin` | AP boot ROM (OpenAVB), 460057 bytes |
| `OpenSEP.bin` | SEP boot ROM (OpenSEP), 147512 bytes |
| `config.plist` | VM config (below) |
| `FirmwareOriginals/iPhone17,1_26.6.2_23G90_Restore/Firmware/.../kernelcache.research.vphone600` | guest kernelcache |

## config.plist (relevant keys)

- `platformType`: `vresearch101`
- `guestProductType`: `iPhone17,1`
- `cpuCount`: `4`
- `memorySize`: `4294967296` (4 GB)
- `romImages.avpBooter`: `OpenAVB.bin`
- `romImages.avpSEPBooter`: `OpenSEP.bin`
- `nvramStorage`: `nvram.bin`
- `sepStorage`: `SEPStorage`
- `diskImage`: `Disk.img`
- `screenConfig`: 1206x2622 @3x, 460ppi

## Identity

- `restore-info.json`: device `iPhone99,11`, iOS `26.6.2` (build `23G90`), cloudOS `26.4` (build `23E5207q`), variant `jb`
- `udid-prediction.txt`: `UDID=0000FE01-97B06A865FBDCF26`, `CPID=0x0000FE01`, `ECID=0x97B06A865FBDCF26`
- `launchpad.json`: bootChain/bundle/guestEnvironment `2.18.0`

vphone-cli ran fine on this M1 Mac mini — no need for the vphone-linux `docs/CREATE-ON-LINUX.md` fallback.

## Running it in hengevm (`--machine vresearch101`)

Boot an APFS clone, never the vphone-cli original:

```bash
V=~/vphone-vms/iphone16pro-henge
./build/hengevm --machine vresearch101 --rom $V/OpenAVB.bin --sep-rom $V/OpenSEP.bin --sep-storage $V/SEPStorage \
  --disk $V/Disk.img --aux $V/nvram.bin --ecid 0x97b06a865fbdcf26 --cpus 4 --ram 4096 --no-pvg
```

Refresh the clone's writable files from the original before a run when a clean start matters
(`cp -c Disk.img nvram.bin SEPStorage` after removing the clone's copies).

M7 (2026-11-26): the same command plus `--display 1206x2622` reaches the lock screen in about four
minutes, drawn by `pvg_lite` + Metal (the iPhone's path). `HV_PVG_SHOT=runs/henge/shots/p
HV_PVG_SHOT_SECS=20` saves the display as PNGs; `HENGE_GXF_LOG=1` logs every GL1 entry and exit.

M4 (2026-11-26): with the SEP, OpenAVB finishes `SEP bootstrap complete`, iBoot runs to
`End of iBoot serial output`, and the next stage stops at `GENTER` (M5). The SEP's own UART lines,
if any, start with `[sep] `.

M3 (2026-11-25): OpenAVB prints `OpenBootROM: SEP RX empty` / `SEP RX timeout` over and over (no SEP
core yet), then after 10001 timeouts reports pvpanic event 2. `HV_VPIOP_LOG=1` logs mailbox traffic,
`HV_AVP_LOG=1` avp-ctrr accesses.
