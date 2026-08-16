# Building and testing

All development runs from this repository; there are no external workspace
dependencies. Two Devenv environments cover the two surfaces, kept separate
so the default shell stays light.

## Host checks and tests (default shell)

```sh
devenv tasks run javascript:test
devenv tasks run host:test
devenv tasks run repository:check   # format, lint, JS checks, host CTest; the CI gate
devenv up                           # web tuner at http://localhost:8787
```

`host:test` builds the Zephyr-free core (`src/core`, `src/source`,
`src/stage`) with the host compiler and runs the CTest suites, including the
versioned fixture replay through the same stage code the firmware compiles.
The default shell contains node, python, gcc, cmake, and ninja only; it never
downloads the Zephyr SDK.

## Firmware smoke build (opt-in firmware shell)

The repository owns its ZMK build workspace. `config/west.yml` pins ZMK by
revision and imports its Zephyr revision, and the tracked `zephyr/module.yml`
adds this repository itself to the build as a Zephyr module. The CI workflow
delegates to `urob/zmk-actions`, which initializes an isolated copy of this
same layout with the equivalent west commands and builds it with the same
west arguments; the steps below are the local equivalent.

One-time workspace setup after cloning:

```sh
devenv shell -P firmware          # opt-in toolchain: west, Zephyr SDK, arm-zephyr-eabi
west init -l config               # creates the gitignored .west/config
west update                       # checks out zmk, zephyr, and modules (gitignored)
west zephyr-export                # registers this workspace's Zephyr with CMake
```

Build the smoke keymap (`config/`: corne_left shield on nice_nano):

```sh
devenv shell -P firmware -- west build -s zmk/app -d build -b nice_nano//zmk -- \
  -DZMK_CONFIG="$(realpath config)" -DSHIELD=corne_left -DZMK_EXTRA_MODULES="$(pwd)"
```

The flashable artifact is `build/zephyr/zmk.uf2`. Rebuild after deleting
`build/` whenever the ZMK config path changes; ZMK caches it and refuses to
switch config directories in place.

The `firmware` profile is defined in `devenv.nix` and pulls its Zephyr
Python environment and SDK through `zephyr-nix`. `devenv.yaml` pins the
Zephyr source input to the revision the pinned ZMK imports. When `west
update` resolves a newer checkout of that revision, keep the input in sync:

```sh
git -C zephyr rev-parse HEAD   # copy into devenv.yaml, then: devenv update
```
