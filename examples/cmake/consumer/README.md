# Plain C consumer

Verifies the installed CMake package and public include layout: includes
`<ultrawidelock/tlv.h>`, links the portable TLV codec, and compiles
`<ultrawidelock/ultrawidelock_hal.h>` with its installed support headers, never
reaching into a module's private `src/`.

Install, then build against that prefix, deriving the compatible SDK series from
the repository's one version source:

```sh
cmake -S . -B build/sdk -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk-install"
cmake --build build/sdk
cmake --install build/sdk
SDK_VERSION="$(sed -n '1p' VERSION)"
cmake -S examples/cmake/consumer -B build/sdk-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/sdk-install" \
  -DULTRAWIDELOCK_REQUIRED_VERSION="${SDK_VERSION%.*}"
cmake --build build/sdk-consumer
```

A vendored source tree consumes the same targets without installing:

```sh
cmake -S examples/cmake/consumer -B build/sdk-consumer-source \
  -DULTRAWIDELOCK_SOURCE_DIR="$PWD"
cmake --build build/sdk-consumer-source
```

The plain CMake package supplies the public C contracts and the portable TLV
target. Complete firmware goes through the Zephyr module or ESP-IDF components.
