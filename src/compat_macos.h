// compat_macos.h — macOS compatibility shims for scrcpy FFI build
// Provides functions missing from the macOS SDK but present in Linux glibc.
// This file is force-included before all other .c files during the FFI build.

#ifndef SCRCPY_FFI_COMPAT_MACOS_H
#define SCRCPY_FFI_COMPAT_MACOS_H

#ifdef __APPLE__

#include <stdlib.h>
#include <stddef.h>

/**
 * reallocarray — Present in OpenBSD/glibc but missing from the macOS SDK.
 * Exposing it here avoids implicit function declaration compilation errors.
 * The implementation is provided by scrcpy's compat.c (HAVE_REALLOCARRAY=0).
 */
extern void* reallocarray(void* ptr, size_t nmemb, size_t size);

#endif // __APPLE__

#endif // SCRCPY_FFI_COMPAT_MACOS_H
