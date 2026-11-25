# Build dependencies (Mac mini, M1, macOS)

`make` and `make test` need these Homebrew packages beyond Xcode's command line tools:

```bash
brew install libslirp cmake
```

- `libslirp` — `host/net_slirp.c` links against it (`pkg-config slirp`); also pulls in `glib` as a dependency.
- `cmake` — `tools/build_ios_restore.sh` uses it to build `libhengerestore.a`.

Verified on this Mac mini (Apple Silicon, macOS Tahoe) on 2026-11-25: with both installed, `make` builds `build/hengevm` clean and `make test` passes all suites (classify, jit_region, operands, jit, mem, mmu, exc, devices, dt_fixup, shutdown).
