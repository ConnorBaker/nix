#pragma once
///@file

#include <filesystem>
#include <optional>
#include <chrono>

#ifndef _WIN32
#  include <sys/resource.h>
#endif

#include "nix/util/types.hh"

namespace nix {

/**
 * Get the current process's user space CPU time.
 */
std::chrono::microseconds getCpuUserTime();

/**
 * If cgroups are active, attempt to calculate the number of CPUs available.
 * If cgroups are unavailable or if cpu.max is set to "max", return 0.
 */
unsigned int getMaxCPU();

// It does not seem possible to dynamically change stack size on Windows.
#ifndef _WIN32
/**
 * Increase the RLIMIT_STACK rlimit if it is currently smaller than `stackSize`.
 * @note Not thread safe. Calls to this should be wrapped in a std::call_once.
 */
void ensureStackSizeAtLeast(size_t stackSize);
#endif

/**
 * Restore the original inherited Unix process context (such as signal
 * masks, stack size).

 * See unix::startSignalHandlerThread(), unix::saveSignalMask().
 */
void restoreProcessContext(bool restoreMounts = true);

/**
 * @return the path of the current executable.
 */
std::optional<std::filesystem::path> getSelfExe();

#ifdef __linux__
/**
 * Whether this process was started through the dynamic loader (`ld.so
 * /path/to/program`), which makes /proc/self/exe the loader: the kernel
 * loaded no interpreter (AT_BASE is 0), yet the program has one.
 */
bool startedThroughLoader();
#endif

} // namespace nix
