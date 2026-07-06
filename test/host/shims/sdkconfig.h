#pragma once
/* Host shim for sdkconfig.h — empty on purpose.
 *
 * Firmware sources that guard behavior with CONFIG_* macros will take the
 * "not defined" branch under host tests. If a specific test needs a
 * CONFIG_* value to be defined, add it here (or better, #define it in the
 * test file before including the firmware header, to keep this shim
 * generic across tests). */
