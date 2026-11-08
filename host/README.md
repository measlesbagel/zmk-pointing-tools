# Host development tools

The host subsystem builds the firmware's pure C processor models as a native
library and exercises them without Zephyr or keyboard hardware.

The shared composable pipeline core and its lifecycle contract are documented
in [`../docs/pipeline-runtime.md`](../docs/pipeline-runtime.md). Host tests also
exercise the generic frame assembler, source metadata, mounting orientation,
and CPI-to-Q16 millimetre normalization.

```sh
cmake -S host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The build produces native composed pipeline replay runners for the noise,
scroll, text-navigation, and cursor fixture kinds. CTest runs the C unit tests
and all versioned fixtures under `host/tests/fixtures` through the JavaScript
replay client.

The native binaries are build artifacts and are never committed or included
in keyboard firmware.
