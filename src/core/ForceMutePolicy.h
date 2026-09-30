#pragma once

// ForceMutePolicy -- who a moderator's force-mute is for. No Qt -- see
// tests/force_mute_policy_test.cpp. SignalingClient extracts the fields and
// owns the side effects (emit forceMuted / remoteMuteChanged).
//
// Wire shape (spreed 24.0.4: CallParticipantModel.forceMute() ->
// webrtc.sendToAll('control', {action:'forceMute', peerId}); simplewebrtc.js
// 'control' handler): the message goes to EVERY peer in the call with the
// payload { action:"forceMute", peerId:<target> }, and only the session named
// in peerId mutes itself -- everyone else marks that peer muted. Under the HPB
// (signaling.js Standalone sendCallMessage) it travels as a top-level
//     { type:"control", control:{ sender, data:<payload> } }
// not inside a "message". TalQ used to look only inside "message" and read
// data.action (always empty there), so a moderator's force-mute never reached
// a TalQ user: the room saw them muted while they kept transmitting. Reading
// only the action (ignoring peerId) would be the opposite failure -- muting
// every TalQ user in the call.

#include <string>

namespace talq {

enum class ForceMuteTarget {
    None,   // not a force-mute, or no target -- ignore
    Self,   // this session was force-muted: actually stop transmitting
    Other,  // another participant was force-muted: mark their tile muted
};

inline ForceMuteTarget classifyForceMute(const std::string &payloadAction,
                                         const std::string &payloadPeerId,
                                         const std::string &ownSessionId)
{
    if (payloadAction != "forceMute" || payloadPeerId.empty())
        return ForceMuteTarget::None;
    if (!ownSessionId.empty() && payloadPeerId == ownSessionId)
        return ForceMuteTarget::Self;
    return ForceMuteTarget::Other;
}

} // namespace talq
