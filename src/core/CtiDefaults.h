#pragma once

// Out-of-the-box defaults for the inbound-call screen-pop.
//
// A branded distribution ships knowing where its own phone system and customer
// system live, so staff never type a URL. The generic build ships knowing
// nothing, because it has no idea what a given site runs — the fields start
// empty and the feature stays dormant until someone fills them in.
//
// These are DEFAULTS, never a lock. Whatever the user sets in Settings wins,
// and an explicit "off" is remembered: QSettings distinguishes "never set"
// from "set to false", so turning the feature off does not silently revert on
// the next launch.

namespace TalQCti {

#ifdef TALQ_BRAND_123NET
// The real endpoints are NOT in this source. They live in the private branding
// store, which CMake puts on the include path only for a branded build — the
// same contract as the update channel in AppSettings.h. A branded build
// without that store fails to compile, which is intended.
#include "brand_cti.inc"
#else
// Open-source build: no endpoints, and the feature is off until configured.
// There is nothing sensitive here because there is nothing here at all.
constexpr auto kServerUrl        = "";
constexpr auto kErpBaseUrl       = "";
constexpr bool kEnabledByDefault = false;
#endif

} // namespace TalQCti
