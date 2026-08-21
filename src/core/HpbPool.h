#pragma once

// Compile-time HPB (signaling server) pool for automatic nearest-server selection.
//
// Brand-aware, mirroring TalQUpdates in AppSettings.h:
//   - Branded build: the real regional pool lives in the PRIVATE branding store,
//     which CMake places on the include path only for a branded build. The client
//     augments the Nextcloud-assigned server with these and picks the nearest.
//   - Generic / open-source build: NO external pool. The client only ever uses the
//     server(s) the user's own Nextcloud hands out — it must never phone home to
//     any third-party infrastructure. kPool is empty (just the terminator).
namespace TalQHpb {
#ifdef TALQ_BRAND_123NET
    #include "brand_hpb_pool.inc"          // defines: static const char *const kPool[]
#else
    static const char *const kPool[] = { nullptr };
#endif
}
