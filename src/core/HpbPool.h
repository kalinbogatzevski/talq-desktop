#pragma once

// Compile-time HPB (signaling server) pool for automatic nearest-server selection.
//
// Brand-aware, mirroring TalQUpdates in AppSettings.h:
//   - Branded (BRAND) build: the real regional pool lives in the PRIVATE store
//     (private/branding/brand/brand_hpb_pool.inc), placed on the include path only
//     for the BRAND build by CMake. The client augments the Nextcloud-assigned
//     server with these and picks the nearest.
//   - Generic / open-source build: NO external pool. The client only ever uses the
//     server(s) the user's own Nextcloud hands out — it must never phone home to
//     BRAND infrastructure. kPool is empty (just the terminator).
namespace TalQHpb {
#ifdef TALQ_BRANDED
    #include "brand_hpb_pool.inc"          // defines: static const char *const kPool[]
#else
    static const char *const kPool[] = { nullptr };
#endif
}
