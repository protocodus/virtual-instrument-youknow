#pragma once

// The owner selected the five-mechanism B audition on 2026-10-05
// (Docs/decisions.md). Select it by default, including non-CMake consumers.
// An explicit zero retains the earlier profile for reference builds.
#ifndef YOUKNOW_HARDWARE_REALISM_CANDIDATE
#define YOUKNOW_HARDWARE_REALISM_CANDIDATE 1
#endif

#if YOUKNOW_HARDWARE_REALISM_CANDIDATE
#include "YouKnowProductHardwareRealism.h"
#else
#include "YouKnowProductFidelity.h"
#endif

namespace youknow
{
#if YOUKNOW_HARDWARE_REALISM_CANDIDATE
using ActiveProductFidelityProfile = ProductHardwareRealismProfile;
#else
using ActiveProductFidelityProfile = ProductFidelityProfile;
#endif
} // namespace youknow
