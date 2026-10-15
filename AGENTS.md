# tesworld

The world-reading library SARP and Oculory share. See README.md for the targets.

## Commands

```sh
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

On Linux the `linux` preset; both need `VCPKG_ROOT`. Warnings are errors.

## Rules

- **No game runtime.** Nothing here knows about SKSE or the game's camera; SARP's renderer stays in SARP. GPU work is
  headless, in `tesworld::gpu`.
- **No globals.** State a caller might want to share (the header cache, archives, asset caches) is an object it owns.
- **No platform headers in public headers.** Windows and POSIX code goes behind `Platform.h`.
- **Game space throughout.** Positions are the game's Z-up units; a consumer changes basis itself.
- **Tests keep `assert` on** in every configuration (`#undef NDEBUG` at the top of each test).
- **`tesworld::nif` is the only target that links nifly**, nifly is the only third-party GPL code, and every other
  dependency is permissive. The rest of tesworld is also MIT for SARP (README, Licence), which only holds while that's
  so: no GPL code from anyone else outside `tesworld::nif`.
