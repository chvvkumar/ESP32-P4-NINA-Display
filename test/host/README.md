# Host-side unit tests

Standalone CMake project for building and running unit tests against firmware
source files on a development machine (Linux, macOS, or Windows), without the
ESP-IDF toolchain. This is separate from `tests/`, which is a Python
stress/soak harness that drives real devices over the network.

## Layout

```
test/host/
  CMakeLists.txt        Standalone CMake project (not part of the ESP-IDF build)
  vendor/cJSON/          cJSON vendored from esp-idf components/json/cJSON
  shims/                 Header/source stand-ins for ESP-IDF and FreeRTOS APIs
  README.md              This file
test/moon/
  test_moon_compute.c    Existing moon-ephemeris test, wired in as test_moon
```

## Building and running

```
cmake -S test/host -B build_host
cmake --build build_host
ctest --test-dir build_host --output-on-failure
```

On Windows, pass a generator if CMake does not pick one up automatically
(for example `-G "MinGW Makefiles"` if using MinGW, or omit `-G` to let
CMake use whatever Visual Studio generator it detects).

## Adding a new test

Use the `add_nina_host_test()` CMake function defined in `CMakeLists.txt`:

```
add_nina_host_test(test_my_module
    SOURCES
        test/host/tests/test_my_module.c
        main/my_module.c
)
```

This links the vendored `cjson` library and the `host_shims` library, and
registers the executable with `ctest`. Add extra sources (a firmware `.c`
file plus any test-local mocks) as needed. Firmware files under `main/`
compile unmodified: no `#ifdef HOST_TEST` branches are needed in firmware
source.

## Shim scope

The shims under `test/host/shims/` exist only to satisfy compilation of
firmware headers and the small set of ESP-IDF/FreeRTOS calls that firmware
source references. They are not a functional reimplementation of ESP-IDF:

- `esp_log.h`, `esp_err.h` are fully functional (log macros print to
  stderr; error codes are plain ints).
- `esp_timer.h` is fully functional: `esp_timer_get_time()` returns a real
  monotonic clock reading by default, or a test-controlled fake value after
  calling `shim_set_time_us()` (reset with `shim_reset_time()`).
- `esp_heap_caps.h` is fully functional: `heap_caps_*` calls route straight
  to the libc heap; capability flags (`MALLOC_CAP_SPIRAM`, etc.) are ignored.
- `freertos/*.h` provide single-threaded stand-ins: mutex take/give always
  succeed immediately, `vTaskDelay` is a no-op, and there is no real
  scheduling or blocking. Do not use these shims for tests that depend on
  actual concurrent task behavior.
- `esp_http_client.h` provides only opaque types and function
  declarations, no implementations. It is enough for a header like
  `main/nina_client_internal.h` to compile, but any `.c` file that actually
  calls `esp_http_client_*` functions will fail to link unless the test
  supplies its own mock implementations of the functions it needs.
- `esp_system.h`, `sdkconfig.h` are intentionally minimal placeholders;
  extend them only as new tests require specific symbols, and prefer
  defining `CONFIG_*` macros in the test file itself over adding them
  globally to the shared `sdkconfig.h` shim.

When a new firmware header pulls in an ESP-IDF header not yet covered here,
add the smallest shim that satisfies the compiler and extend this list.
