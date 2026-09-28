# Current validation baseline

The public production tree intentionally excludes historical gate scripts,
patch backups, build products, and one-off hardware instrumentation.

Validated functional baseline before production cleanup (2026-09-27):

- NinaLink bridge/reliability/discovery/cache/session semantics through B5.x
- Direct-NDP low-power Home Assistant path
- NinaLink Home Assistant semantic entities and writable behavior controls
- B7.6f2m6c2b Hall CAP_SET authoritative readback/reconcile: PASSED
- Home Assistant integration 0.10.22 baseline

Production cleanup must not change frozen NinaLink RF frames, the VM ABI,
bootloader layout, or Home Assistant semantic behavior.

Before tagging a release, build from a clean checkout and record:

```bash
make clean
make -j$(nproc)
make size
make size-detail
git diff --check
```
