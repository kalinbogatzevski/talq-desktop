#pragma once

// Hidden, headless end-to-end check of the REAL CallManager:
//
//     talq.exe --selftest-call [--log <file>] [--callee <talq-call-test.exe>]
//                              [--rounds N] [--early-rounds N] [--cold-session]
//
// The call harness (tests/call_test.cpp) drives SignalingClient and the
// pipelines directly and has NO CallManager, so nothing automatic exercised the
// hang-up teardown, the publisher-sid guard or the ICE-new watchdog. This mode
// builds ApiClient + SignalingClient + MediaDeviceManager + CallManager the way
// main() does -- except that it never calls setConversations(), so
// CallManager::isOneToOneCall() is always false and every self-test call takes the
// group-call branches, and it turns idle HPB re-homing off to keep one signaling
// session across rounds. It logs in as a TEST BOT (it refuses any other account
// name), calls a second bot that is a harness AUTO_ANSWER process this mode
// spawns, and checks the captured log. The room and server come from the
// environment (TALQ_TEST_TOKEN, TALQ_TEST_SERVER); only the two account NAMES are
// checked, so a mis-set room is not caught. Exit code 0 = every assertion held,
// 1 = at least one failed, 2 = not runnable (bad arguments / missing setup).
//
// It is dispatched from main() BEFORE the single-instance check, the log
// rotation and QApplication, so it can run next to a live TalQ without touching
// its log, its lock or its window. Its QSettings writes are redirected into a
// scratch registry hive for the life of the process (RegOverridePredefKey), so
// the installed app's settings are never written or changed (main() still reads
// the one Diagnostics/detailedLogging flag before it dispatches here).
//
// Limits: it is a healthy-path test plus one race. The publisher-sid guard and the
// ICE-new watchdog need a publisher rebuild / ICE stuck at "new", which a healthy
// call never produces and no public CallManager method triggers, so they are NOT
// exercised here. RegOverridePredefKey remaps ALL of HKEY_CURRENT_USER for the
// process (not just QSettings), so WinINet sees an empty Internet Settings hive:
// the self-test always runs with "no system proxy", never exercises the proxy
// paths and will not connect on a proxy-only network.
int talqRunCallSelfTest(int argc, char **argv);
