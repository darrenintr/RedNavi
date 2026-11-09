// SPDX-License-Identifier: BSD-3-Clause
// Declaration-only IOKit surface for host tests of the actual driver.
// Test translation units provide recording or no-op implementations.
#pragma once
struct IOLock;
extern "C" {
IOLock *IOLockAlloc(void);
void IOLockFree(IOLock *);
void IOLockLock(IOLock *);
void IOLockUnlock(IOLock *);
void IOLog(const char *, ...);
}
