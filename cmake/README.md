# Shared CMake helpers

`ultrawidelock_roles.cmake` reads the declarative source-role manifests and
returns resolved source lists to Zephyr and ESP-IDF builds, so source membership
stays in one place while each framework creates its own targets.

`UltraWideLockConfig.cmake.in` defines the installed plain-CMake package consumed
through `find_package(UltraWideLock CONFIG REQUIRED)`. Its exported targets come
from the top-level `CMakeLists.txt`.

Application entry points stay in their application directories; user-facing
build commands stay in the top-level `Makefile`.
