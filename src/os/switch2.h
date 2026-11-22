// libnx (libounce) for the Switch 2 runtime code. Its Mutex type is renamed NxMutex here:
// the runtime has its own Mutex (mutex.h).
#pragma once

#if defined(__SWITCH2__)
#define Mutex NxMutex
#include <switch.h>
#undef Mutex
#endif
