// Precompiled header for the runtime. Adapted from Unleashed Recompiled's stdafx.h
// with the graphics, audio and UI dependencies removed.
#pragma once

#define NOMINMAX

#if defined(__linux__)
#include <unistd.h>
#include <pwd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <numeric>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <xbox.h>
#include <xxhash.h>
#include <ankerl/unordered_dense.h>
#include <ppc_recomp_shared.h>
#include <o1heap.h>
#include <fmt/core.h>

#include "framework.h"
#include "mutex.h"

#ifndef _WIN32
#include <sys/mman.h>
#endif
