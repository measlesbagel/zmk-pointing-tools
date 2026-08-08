# Host development tools

The host subsystem builds the firmware's pure C processor models as a native
library and exercises them without Zephyr or keyboard hardware.

The shared composable pipeline core and its lifecycle contract are documented
in [`../docs/pipeline-runtime.md`](../docs/pipeline-runtime.md). Host tests also
exercise the generic frame assembler and identity cursor stage used by the
minimal ZMK boundary, plus source metadata, mounting orientation, and CPI-to-Q16
millimetre normalization.

```sh
cmake -S host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The build produces native scroll and text-navigation replay runners. CTest
runs the C unit tests and all versioned fixtures under `host/tests/fixtures`
through the JavaScript replay client.

Use the import utility manually to turn part of a tuner export into a fixture:

```sh
node host/replay/import.js \
  --input ~/Downloads/zmk-pointing-trace.json \
  --stream 0:0 \
  --template host/tests/fixtures/left-split-transport.json \
  --output /tmp/new-fixture.json
```

The native binaries are build artifacts and are never committed or included
in keyboard firmware.
