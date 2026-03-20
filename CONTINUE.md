# Continue Prompt for Next Session

## Active Bug: Memory Leak

When opening Tiara's chat (token: 89ogh57q, 1:1), memory grows exponentially (40MB to 1.6GB in seconds). Ilko's chat is stable. The leak exists even with gst_init disabled. Likely cause: signal cascade in onContentHeightChanged + positionViewAtIndex creating infinite delegate loop for certain message patterns (Tiara's chat has many reply messages with embedded parent objects). The v0.6.1 scroll code (autoScrolling + onContentHeightChanged) was restored but not tested yet.

## Setup on New Machine

1. `git pull` in `C:\src\talk-desktop-qt`
2. Install MSYS2 (`C:\msys64`), then run in MSYS2 shell:
   ```
   pacman -S mingw-w64-x86_64-gstreamer mingw-w64-x86_64-gst-plugins-base mingw-w64-x86_64-gst-plugins-good mingw-w64-x86_64-gst-plugins-bad mingw-w64-x86_64-gst-plugins-ugly
   ```
3. Copy 13 plugin DLLs from `C:\msys64\mingw64\lib\gstreamer-1.0\` to `C:\build\talk-qt\gst-plugins\`:
   - libgstcoreelements, libgstaudioconvert, libgstaudioresample, libgstopus, libgstrtp, libgstrtpmanager, libgstwasapi2, libgstautodetect, libgstwebrtc, libgstdtls, libgstnice, libgstsrtp, libgstapp
4. CMake configure:
   ```
   cmake -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_BUILD_TYPE=Release -DTALQ_BRAND=123NET -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DGSTREAMER_ROOT=C:/msys64/mingw64 C:/src/talk-desktop-qt
   ```
5. Build: `cmake --build . --target talq`
6. Run via `C:\build\talk-qt\run.bat` (sets MSYS2 first in PATH + minimal plugin scanning)

## What's Done

- v0.6.1 released (thread detection fix, scroll fix, UI improvements)
- v0.7.0 calls: Phase 1 (GStreamer), Phase 2 (SignalingClient + CallManager), Phase 3 (webrtcbin + call UI) — all compile and link
- Call button visible in 1:1 chat headers
- IncomingCallPopup and CallWindow QML created
- gst_init currently disabled for stability

## What's Next

1. Fix the memory leak (priority!)
2. Re-enable gst_init
3. Test an actual call between two TalQ instances
4. Phase 4: Video + screen sharing
5. Phase 5: UI polish
