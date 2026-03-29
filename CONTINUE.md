# TalQ v0.13.1 Continue Prompt

## Current status
Full QWidget app with working audio + video calls via HPB/Janus MCU. Released v0.13.1.

## What was done (v0.13.0–v0.13.1, 2026-03-28/29)

### Audio calls — fully working with MCU/HPB
- Fixed OPUS codec (was MULTIOPUS, Janus rejected it)
- Added dummy 16x16 black VP8 track for audio-only calls (Janus MCU requires video from all publishers)
- Accept/Decline buttons now work (setUserActionReady wiring)
- Hangup sends DELETE /call with all=true to end call for both parties
- Initial media state broadcast when remote peer joins (not just on ICE)
- Stale call detection: first conversation load seeds call state silently
- Re-request subscriber stream when remote enables video mid-call

### Video calls — bidirectional working
- Remote video display in CallDialog via QPainter (I420→QImage conversion)
- Camera uses VP8 codec (matches Janus MCU, was H264)
- Auto-negotiate camera caps — no hardcoded format/resolution/framerate
- Camera preview via local VideoFrameProvider + small overlay widget
- Dialog auto-expands when remote enables camera, shrinks when disabled
- Remote avatar replaces video when camera is muted
- Skip dummy 16x16 MCU frames (only show real video >32px)

### Remote media state tracking
- Parse incoming mute/unmute signaling messages (remoteMuteChanged signal)
- Show 🔇 indicator on peer name when remote mic is muted
- Track remoteVideoMuted/remoteAudioMuted properties

### Call dialog UX
- Circular avatar of remote party in header
- Mic emoji 🎤 (was speaker 🔊)
- circleButtonStyle() helper for consistent button styling
- Dialog height 300x340 (audio) / 400x500 (video)
- Publisher ICE status doesn't overwrite "Connected" when Active

### Code review fixes (critical)
- Ring timeout bypasses userActionReady (was stalling call forever)
- SubscribePipeline::onNewVideoSample uses QPointer guard (was use-after-free)
- leaveCallOnServer checks m_joinedCall (was double-DELETE on decline)
- STUN URL prefix replacement uses mid() (was corrupting mid-string matches)
- Video provider flags reset on provider change (was freezing after camera toggle)

### Code simplification
- Extracted helpers: circleButtonStyle, indexOfToken, makeCandidateJson, callFlags, onAudioLevelUpdated
- Removed dead m_jpegDec code path and unused m_busWatchId
- Net -67 lines

## Next steps
- Camera doesn't work on this laptop (mfvideosrc COM/STA issue) — test on work laptop
- messageModel.count Q_PROPERTY (count used in QML but not a declared property)

## Architecture notes

### Call flow (MCU mode)
1. startCall → POST /call/{token} → join call on server
2. PublishPipeline: always starts with dummy 16x16 VP8, camera replaces if available
3. Offer sent to own session (HPB creates Janus publisher room)
4. Remote joins → requestOffer → SubscribePipeline receives remote audio/video
5. ICE: STUN + TURN servers from /signaling/settings
6. Media state broadcast via signaling mute/unmute messages
7. Hangup: DELETE /call/{token}?all=true + teardown pipelines

### Video display
- VideoFrameProvider converts I420 GstSample → QImage via BT.601 YUV→RGB
- CallDialog::VideoWidget paints QImage with QPainter (aspect-ratio preserving)
- Remote video shown only when frames >32px and remote video not muted
- Local preview: small 120x90 overlay positioned at bottom-right of remote video
