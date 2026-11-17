// Quick Start (F1 > Enhancements > General; docs/FEATURES.md "Quick Start").
//
// What a normal launch with a saved town does (USA rev 1, measured; every version the same code):
//   boot (no stage, byte r13-0x6384 = 68): NANDInit, the save worker's boot check reads rvforest.dat
//     (result word r13-0x6930: 0 running -> 1 done; docs/NAND.md), wrist-strap screen (waits for A),
//     ESRB notice (USA), then fn_80162C40(DM_TITLE).
//   DM_TITLE (stage 0x38): the title over a fly-over of the saved town (d_title_demoNP); A ->
//     DM_PL_SEL.
//   DM_PL_SEL (stage 0x3B): Rover in the dark (d_a_npc_sp_player_selectNP, SYS_Start.bmg): "Ah! Came
//     to play" (msg 0x01), "Right now, in <town>, it's ... How about we get started?" (0x02, choices
//     Yeah! / DS Suitcase / Before playing / Other things), "Now, hang on... You are...?" (0x04) with
//     the resident list (REL fn_24_1BF4: one row per resident, each with the message Rover answers
//     with; then "someone new" and "back" rows while the town has room), Rover's greeting for that
//     resident (0x33..0x4B, by days away, birthday...; 0x4F = the resident is away in the DS
//     Suitcase), "Preparing saved data." (0x65: the session-start save, rvforest.dat rewritten via
//     /tmp/sys) and "Thanks for waiting!" (0x51..0x58), then fn_80163064(FIELD, the resident's house
//     door, type 0xB): the town loads and the resident steps out of the house.
//
// What Quick Start does: nothing is skipped or written by the port. The same code runs, with
//   - the frames not shown and the VI retrace / alarms unpaced (like the CF_FF test aid, but the next
//     retrace comes as soon as every guest thread waits: hle_vi.cpp, sched::cpuIdle), sound muted,
//     an empty black frame shown every 100 ms so the window stays responsive; it starts while the
//     shader cache loads (game::runDuringShaderHold) and its first shown frame waits for that;
//   - the modelled NAND / disc latencies scaled to 0 (game::ioLatencyScale; CF_QUICK_START_IO);
//   - Wii Remote 1 driven by the port: A pressed every other frame (strap screen, title, Rover's
//     lines and the "Yeah!" choice, the box's default top row), the pointer away from the screen,
//     and in the resident list Down / Up until the box's cursor (box + 0x934; rows + 0x930, built
//     by fn_8015D27C) is on the chosen resident, then A;
//   - only expected screens: every message Rover starts (fn_801A4F68, msg manager + 0x6C84) must be
//     one of the normal start's; anything else (the DS Suitcase question, a reply to a choice we did
//     not mean) stops Quick Start and the player sees that screen and answers it. The same when the
//     save worker reports an error, the resident list is not what the save says, a stage other than
//     these appears, Shift / B is held, or it takes too long.
// It ends when the town's circle wipe starts to open (mWipeFader_c::draw, frame +0x44 below 0xFF:
// the town has loaded behind the closed circle, unseen): the first frame shown is the resident at
// the door. No presses after the stage change to the town was requested.
#include "game/cf_quick_start.h"

#include "game/cf_save_data.h"
#include "port/install.h"
#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/ios.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "recomp/wiimote.h"

#include <imgui.h>
#include <SDL3/SDL_keyboard.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
namespace quickstart {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kProcessStart = Clock::now();  // (static init: the program's start)
double msSinceStart() { return std::chrono::duration<double, std::milli>(Clock::now() - kProcessStart).count(); }

// USA addresses (VA() maps them; the code is the same in all four versions).
u32 stageAddr() { return VA(0x8074B5BC); }        // r13-0x6384: current stage (top byte)
u32 stageMgr() { return VA(0x805FB218); }         // stage manager; +0x1F = a change is pending
u32 saveResultAddr() { return VA(0x8074B010); }   // r13-0x6930: the save worker's result word
constexpr u32 kField = 0x00, kDmTitle = 0x38, kDmPlSel = 0x3B, kNoStage = 0x44;
constexpr u32 kBoxRows = 0x930, kBoxCursor = 0x934;
constexpr u32 kMsgYouAre = 0x04;

enum class Phase { Off, Running, Done, Stopped };
std::atomic<Phase> sPhase{Phase::Off};
std::atomic<bool> sDecided{false};  // onBoot has run
std::atomic<u32> sButtons{0};  // what Wii Remote 1 holds this frame (game bits)

struct Plan {
    std::string town, resident;
    int slot = -1, row = 0, residents = 0;
};
Plan sPlan;
std::string sWhy;  // why this launch did (not) start quickly, for F1 (guarded by sWhyM)
std::mutex sWhyM;
void setWhy(const std::string& s) {
    std::lock_guard lk(sWhyM);
    sWhy = s;
}

// Progress (game thread).
u32 sStartFrame = 0, sLastStage = ~0u, sMsg = ~0u, sMsgMgr = 0, sBox = 0;
bool sResidentBox = false, sBoxOpening = false, sPressedLast = false;
std::atomic<bool> sWipeOpening{false};  // in the town: the circle wipe has started to open
u32 sFieldFrame = 0, sThanksFrame = 0;
double sThanksMs = 0;
double sStartMs = 0, sLastShownMs = 0;

// An empty frame is shown this often while running (window events; CF_QUICK_START_SHOW=<ms>).
double showEveryMs() {
    static const double ms = [] {
        const char* v = std::getenv("CF_QUICK_START_SHOW");
        return v ? std::max(1.0, std::atof(v)) : 100.0;
    }();
    return ms;
}

// Test aid CF_QUICK_START_PACED=1: the same presses at console speed with every frame shown and
// the modelled latencies: the fastest a player can go through the normal start (the comparison
// for Quick Start's timing).
bool paced() {
    static const bool on = [] {
        const char* v = std::getenv("CF_QUICK_START_PACED");
        return v && *v == '1';
    }();
    return on;
}

bool logOn() {
    static const bool on = std::getenv("CF_QUICK_START_LOG") != nullptr;
    return on;
}

const char* stageName(u32 s) {
    switch (s) {
    case kField: return "FIELD";
    case kDmTitle: return "DM_TITLE";
    case kDmPlSel: return "DM_PL_SEL";
    case kNoStage: return "boot";
    default: return "?";
    }
}

// Messages of SYS_Start.bmg the normal start of a saved resident shows (every language: the ids
// are the file's, the same on every disc).
bool expectedMessage(u32 id) {
    if (id >= 0x01 && id <= 0x04) return true;  // came to play / get started? / you are...?
    if (id >= 0x33 && id <= 0x4B) return id != 0x4F;    // greetings (0x4F: away in the DS Suitcase)
    if (id >= 0x51 && id <= 0x58) return true;          // "Thanks for waiting!"
    return id == 0x65;                                  // "Preparing saved data."
}

port::GameVersion runningVersion() {
    switch (cfsave::regionOfGameCode(u32(ios::titleId()))) {
    case cfsave::kEurope: return port::GameVersion::PAL;
    case cfsave::kJapan: return port::GameVersion::JPN;
    case cfsave::kKorea: return port::GameVersion::KOR;
    default: return port::GameVersion::USA;
    }
}
std::string& residentSetting(port::GameVersion v) {
    auto& q = port::settings().quickStart;
    switch (v) {
    case port::GameVersion::PAL: return q.residentEurope;
    case port::GameVersion::JPN: return q.residentJapan;
    case port::GameVersion::KOR: return q.residentKorea;
    default: return q.residentUsa;
    }
}

// The save's residents as Rover lists them: slots with a player, in slot order.
struct Resident {
    int slot;
    std::string key, name;  // key "<town id>-<player id>" (hex)
};
struct SaveView {
    bool read = false;
    cfsave::SaveInfo info;
    std::vector<Resident> residents;
};
std::string hex4(u32 v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v & 0xFFFF);
    return b;
}
SaveView viewSave(const std::vector<u8>& save) {
    SaveView v;
    v.read = true;
    v.info = cfsave::inspectSave(save);
    if (!v.info.sizeOk) return v;
    for (u32 i = 0; i < cfsave::kResidents; i++) {
        const u8* pd = save.data() + cfsave::kResidentBase + i * cfsave::kResidentSize;
        const u32 player = u32(pd[cfsave::kPlayerStruct]) << 8 | pd[cfsave::kPlayerStruct + 1];
        if (!player || player == 0xFFFF) continue;
        const u32 town = u32(pd[cfsave::kTownStruct]) << 8 | pd[cfsave::kTownStruct + 1];
        v.residents.push_back({int(i), hex4(town) + "-" + hex4(player), ""});
    }
    for (size_t i = 0; i < v.residents.size() && i < v.info.residents.size(); i++) v.residents[i].name = v.info.residents[i];
    return v;
}
bool readSave(std::vector<u8>& out) { return ios::readHostFile(ios::titleDataDir() + "/rvforest.dat", out); }

// Shift (keyboard) held: start normally this time.
bool bypassKeyHeld() {
#ifdef _WIN32
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) return true;
#endif
    return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
}
std::atomic<bool> sBypassB{false};  // B held on Remote 1 (its bound inputs), seen by filterRemote

void stop(const char* why, u32 frame) {
    if (sPhase.load() != Phase::Running) return;
    sPhase = Phase::Stopped;
    sButtons = 0;
    gx::g_discardDraws = false;
    const double ms = msSinceStart() - sStartMs;
    log("[quickstart] stopped at guest frame %u (stage %s, message 0x%X) after %.0f ms: %s; the game continues "
        "normally from here",
        frame, stageName(sLastStage), sMsg == ~0u ? 0 : sMsg, ms, why);
    setWhy(std::string("This launch: Quick Start stopped (") + why + "); the game went on normally.");
}

}  // namespace

// ---- hooks ---------------------------------------------------------------------------------------
// fn_801A4F68(msgManager, id): a talk's next message (sth id, +0x6C84). Every line Rover starts,
// also the ones a choice or a random branch leads to.
HLE(fn_801A4F68) {
    const u32 mgr = c.r[3], id = c.r[4] & 0xFFFF;
    callOriginal(c, "fn_801A4F68");
    if (sPhase.load(std::memory_order_relaxed) != Phase::Running) return;
    if (logOn()) log("[quickstart] message 0x%X (manager %08X)", id, mgr);
    sMsgMgr = mgr;
    // 0 = the line closed (the engine clears the id before the next one, and right after a choice
    // box comes up): the talk is still at the last line, e.g. "You are...?" with its list.
    if (id == 0) return;
    sMsg = id;
    if (id != kMsgYouAre) sResidentBox = false;
}
// fn_8015C83C(box, countdown): called every frame while a choice box opens (its rows scale / fade
// in for about 7 frames; the box takes input once it has opened, cursor +0x934 >= 0).
HLE(fn_8015C83C) {
    const u32 box = c.r[3];
    callOriginal(c, "fn_8015C83C");
    if (sPhase.load(std::memory_order_relaxed) != Phase::Running || box != sBox) return;
    if (logOn() && !sBoxOpening) log("[quickstart] choice box %08X opening", box);
    sBoxOpening = true;
}
// fn_8015D27C(box, ...): a choice box is built (rows at +0x930, cursor +0x934 = -1 until it has
// opened). The one built while "You are...?" is current is the resident list.
HLE(fn_8015D27C) {
    const u32 box = c.r[3];
    callOriginal(c, "fn_8015D27C");
    if (sPhase.load(std::memory_order_relaxed) != Phase::Running) return;
    sBox = box;
    sBoxOpening = false;
    sResidentBox = sMsg == kMsgYouAre;
    if (logOn())
        log("[quickstart] choice box %08X built (%d rows) at message 0x%X%s", box, s32(rd32(box + kBoxRows)), sMsg,
            sResidentBox ? ": the resident list" : "");
}

// ---- start ---------------------------------------------------------------------------------------
void onBoot(Ctx&) {
    struct Decided {
        ~Decided() { sDecided = true; }
    } decided;
    const port::GameVersion ver = runningVersion();
    bool enabled = port::settings().quickStart.enabled;
    std::string want = residentSetting(ver);  // "" = first resident
    int wantSlot = -1;
    std::string wantName;
    // Test aid CF_QUICK_START=0|1|slot:<n>|name:<name>: off / on with the setting's resident / on
    // with that save slot (0..3) or resident name.
    if (const char* e = std::getenv("CF_QUICK_START")) {
        const std::string v = e;
        enabled = v != "0";
        if (v.rfind("slot:", 0) == 0) wantSlot = std::atoi(v.c_str() + 5), want.clear();
        else if (v.rfind("name:", 0) == 0) wantName = v.substr(5), want.clear();
    }
    if (!enabled) {
        setWhy("");
        return;
    }
    const auto off = [](const std::string& why) {
        log("[quickstart] not used this launch: %s", why.c_str());
        setWhy("This launch: normal start (" + why + ").");
    };
    if (bypassKeyHeld()) return off("Shift was held at the start");
    std::vector<u8> save;
    if (!readSave(save)) return off("there is no save yet");
    const SaveView v = viewSave(save);
    if (!v.info.sizeOk) return off("the save has the wrong size");
    if (!v.info.badChecksums.empty()) return off("the save is damaged (" + v.info.problem() + ")");
    if (v.residents.empty()) return off("the town has no resident yet");
    int pick = -1;
    for (size_t i = 0; i < v.residents.size(); i++) {
        const Resident& r = v.residents[i];
        if ((wantSlot >= 0 && r.slot == wantSlot) || (!wantName.empty() && r.name == wantName) ||
            (!want.empty() && r.key == want))
            pick = int(i);
    }
    if (want.empty() && wantSlot < 0 && wantName.empty()) pick = 0;
    if (pick < 0) return off("the chosen resident no longer lives in " + v.info.town);
    sPlan.town = v.info.town;
    sPlan.resident = v.residents[size_t(pick)].name;
    sPlan.slot = v.residents[size_t(pick)].slot;
    sPlan.row = pick;
    sPlan.residents = int(v.residents.size());
    sStartMs = msSinceStart();
    sLastShownMs = 0;
    sStartFrame = game::guestFrames();
    sPhase = Phase::Running;
    log("[quickstart] on: town %s, resident %s (save slot %d, row %d of %d residents), %.0f ms after the start",
        sPlan.town.c_str(), sPlan.resident.c_str(), sPlan.slot, sPlan.row, sPlan.residents, sStartMs);
    setWhy("This launch: Quick Start as " + sPlan.resident + ".");
}

int onGuestFrame(Ctx&, u32 frame) {
    if (sPhase.load(std::memory_order_relaxed) != Phase::Running) return -1;
    const double now = msSinceStart();
    const u32 stage = rd32(stageAddr()) >> 24;
    if (stage != sLastStage) {
        log("[quickstart] stage %s (%u) at guest frame %u, %.0f ms", stageName(stage), stage, frame, now);
        sLastStage = stage;
    }
    const auto quit = [&](const char* why) {
        stop(why, frame);
        return -1;
    };
    if (bypassKeyHeld() || sBypassB.load()) return quit("Shift / B held");
    if (now - sStartMs > (paced() ? 120000.0 : 30000.0) || frame - sStartFrame > 60000) return quit("it took too long");
    const s32 saveResult = s32(rd32(saveResultAddr()));
    if (saveResult == 2 || saveResult == 3 || saveResult == 5 || saveResult >= 6 || saveResult < 0)
        return quit("the save could not be read");

    u32 buttons = 0;
    const bool pulse = !sPressedLast;  // a button is pressed every other frame (each press a new trigger)
    if (stage == kField) {
        // The town loads behind the closed circle (frame 0xFF: a black quad); still unseen and
        // unpaced, no buttons, until the circle starts to open around the resident at the door.
        if (!sFieldFrame) sFieldFrame = frame;
        if (!sWipeOpening.load() && frame - sFieldFrame < 1200 && now - sStartMs < (paced() ? 120000.0 : 30000.0)) {
            sPressedLast = false;
            sButtons = 0;
            if (paced()) return 0;
            gx::g_discardDraws = true;
            if (now - sLastShownMs >= showEveryMs()) {
                sLastShownMs = now;
                return 0;
            }
            return 1;
        }
        sPhase = Phase::Done;
        sButtons = 0;
        gx::g_discardDraws = false;
        log("[quickstart] in town at guest frame %u, %.0f ms after the start (%.0f ms of Quick Start)", frame, now,
            now - sStartMs);
        char t[32];
        std::snprintf(t, sizeof t, "%.1f", now / 1000.0);
        setWhy("This launch: Quick Start as " + sPlan.resident + ", in town " + t + " s after the program started.");
        return -1;
    } else if (stage == kNoStage || stage == kDmTitle) {
        if (pulse) buttons = wiimote::kBtnA;
    } else if (stage == kDmPlSel) {
        if (sMsg != ~0u && !expectedMessage(sMsg)) return quit("Rover asked something Quick Start does not answer");
        const bool changing = rd8(stageMgr() + 0x1F) != 0;  // the change to the town is under way
        // After "Thanks for waiting!" (0x53..0x58, the line the 0x52 branch picks; 0x52 itself is
        // queued before the save) the bus leaves for the town at once. A resident without a house
        // yet (moved in, the house not bought) stays in Rover's room instead: shown as is.
        if (sMsg >= 0x53 && sMsg <= 0x58 && !changing) {
            if (!sThanksFrame) sThanksFrame = frame, sThanksMs = now;
            if (frame - sThanksFrame > 600 && now - sThanksMs > 1500.0)
                return quit("Rover did not send the resident to the town (no house yet?)");
        }
        if (changing) {
            buttons = 0;
        } else if (sResidentBox) {
            const s32 rows = s32(rd32(sBox + kBoxRows)), cursor = s32(rd32(sBox + kBoxCursor));
            const s32 wantRows = sPlan.residents + (sPlan.residents < 4 ? 2 : 1);
            if (logOn()) {
                static u32 lastLog = 0;
                if (frame - lastLog >= 30) {
                    lastLog = frame;
                    log("[quickstart] resident list f%u: rows %d cursor %d (+0x938 %d, +0x9A0 %d, +0x9A4 %d)", frame, rows, cursor,
                        s32(rd32(sBox + 0x938)), s32(rd32(sBox + 0x9A0)), s32(rd32(sBox + 0x9A4)));
                }
            }
            if (!sBoxOpening) {
                // "You are...?" is still being written: A shows the rest of it; the list then
                // opens (no presses from its first opening frame on, so none can take a row).
                if (pulse) buttons = wiimote::kBtnA;
            } else if (cursor >= 0) {  // opened: takes input
                if (rows != wantRows) return quit("Rover's resident list is not the save's");
                if (pulse) {
                    if (cursor < sPlan.row) buttons = wiimote::kBtnDown;
                    else if (cursor > sPlan.row) buttons = wiimote::kBtnUp;
                    else buttons = wiimote::kBtnA;
                }
                if (logOn() && buttons) log("[quickstart] resident list: cursor %d, want %d -> %04X", cursor, sPlan.row, buttons);
            }
        } else if (pulse) {
            buttons = wiimote::kBtnA;
        }
    } else {
        return quit("an unexpected scene appeared");
    }
    sPressedLast = buttons != 0;
    sButtons = buttons;

    // Not shown, except an empty (black) frame every 100 ms so the window keeps answering.
    if (paced()) return 0;
    gx::g_discardDraws = true;
    if (now - sLastShownMs >= showEveryMs()) {
        sLastShownMs = now;
        return 0;
    }
    return 1;
}

bool running() { return sPhase.load(std::memory_order_relaxed) == Phase::Running; }

bool runDuringShaderHold() {
    if (paced()) return false;
    const Phase p = sPhase.load(std::memory_order_relaxed);
    if (p == Phase::Running) return !std::getenv("CF_QUICK_START_HOLD") || *std::getenv("CF_QUICK_START_HOLD") != '0';
    if (p != Phase::Off || sDecided.load()) return false;
    static const bool sNoOverlap = [] {  // CF_QUICK_START_HOLD=0 (A/B): wait for the shaders first
        const char* v = std::getenv("CF_QUICK_START_HOLD");
        return v && *v == '0';
    }();
    if (sNoOverlap) return false;
    // Not booted yet: the guest decides in onBoot (save checks); start it if Quick Start is wanted.
    const char* e = std::getenv("CF_QUICK_START");
    return e ? std::string(e) != "0" : port::settings().quickStart.enabled;
}

void noteWipeDraw(u32 fader) {
    if ((rd32(stageAddr()) >> 24) != kField) return;
    const u32 f = rd8(fader + 0x44);  // dWipeFader_c frame (fn_8017D0A4); 0xFF = fully closed
    // (CF_QUICK_START_LOG, any launch: when the circle first opens on the town, for timings.)
    static bool sOpenedOnce = false;
    if (logOn() && f != 0xFF && !sOpenedOnce) {
        sOpenedOnce = true;
        log("[quickstart] the town's circle opens %.0f ms after the program started (guest frame %u)", msSinceStart(),
            game::guestFrames());
    }
    if (!running()) return;
    if (logOn() && !sWipeOpening.load()) log("[quickstart] town wipe frame %u", f);
    if (f != 0xFF) sWipeOpening = true;
}

double retraceSpeed() { return running() && !paced() ? 1000.0 : 1.0; }

double ioLatencyScale() {
    static const double scale = [] {
        const char* v = std::getenv("CF_QUICK_START_IO");
        return v ? std::max(0.0, std::atof(v)) : 0.0;
    }();
    return running() && !paced() ? scale : 1.0;
}

void filterRemote(int chan, wiimote::State& st) {
    if (chan != 0 || !running()) return;
    if (st.connected && (st.buttons & wiimote::kBtnB)) sBypassB = true;
    st.connected = true;
    st.buttons = sButtons.load(std::memory_order_relaxed);
    st.pointerValid = false;  // pointing away: choice boxes keep the game's own cursor
    st.stickX = st.stickY = 0;
}

void drawOverlay() {
    if (!running()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 255));
}

// ---- F1 > Enhancements > General -----------------------------------------------------------------
void menuSection(bool& dirty) {
    auto& q = port::settings().quickStart;
    ImGui::SeparatorText("Start-up");
    dirty |= menu::Checkbox("Quick Start", &q.enabled);
    ImGui::SetItemTooltip(
        "From the next launch on, the game opens straight into your town: the resident below steps out\n"
        "of their house. The wrist-strap screen, the title and Rover's questions still run, unseen and at\n"
        "full speed, with the port answering them, so the game loads, checks and saves exactly as when\n"
        "you pick your resident yourself (\"Preparing saved data\" included). Mr. Resetti, letters, events\n"
        "and the new day all happen in town as usual.\n"
        "Hold Shift (or B) while the game starts to get the normal start once.\n"
        "Starts normally by itself when there is no save yet, the save is damaged, the chosen resident\n"
        "has moved out, or Rover has a question only you should answer (e.g. a resident who is away in\n"
        "the DS Suitcase).");

    // The save's residents, re-read every 2 s while this is drawn.
    static SaveView sView;
    static Clock::time_point sRead{};
    if (sRead == Clock::time_point{} || Clock::now() - sRead > std::chrono::seconds(2)) {
        sRead = Clock::now();
        std::vector<u8> save;
        sView = readSave(save) ? viewSave(save) : SaveView{};
    }
    std::string reason;
    if (!sView.read) reason = "no save yet";
    else if (!sView.info.sizeOk || !sView.info.badChecksums.empty()) reason = "the save is damaged";
    else if (sView.residents.empty()) reason = "the town has no resident yet";
    std::string& want = residentSetting(runningVersion());
    ImGui::BeginDisabled(!q.enabled || !reason.empty());
    std::string preview = "First resident";
    int current = -1;
    for (size_t i = 0; i < sView.residents.size(); i++)
        if (!want.empty() && sView.residents[i].key == want) current = int(i), preview = sView.residents[i].name;
    if (want.empty() && !sView.residents.empty()) preview = "First resident (" + sView.residents[0].name + ")";
    else if (!want.empty() && current < 0) preview = "(moved out: " + want + ")";
    ImGui::SetNextItemWidth(260.0f * ImGui::GetFontSize() / 13.0f);
    if (ImGui::BeginCombo("Play as##quickstart", preview.c_str())) {
        if (ImGui::Selectable("First resident", want.empty())) want.clear(), dirty = true;
        for (size_t i = 0; i < sView.residents.size(); i++) {
            ImGui::PushID(int(i));
            if (ImGui::Selectable(sView.residents[i].name.c_str(), int(i) == current)) want = sView.residents[i].key, dirty = true;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Which resident Quick Start picks when Rover asks \"You are...?\" (this version's save%s%s).",
                          reason.empty() ? "" : "; unavailable: ", reason.c_str());
    if (!reason.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", reason.c_str());
    }
    {
        std::lock_guard lk(sWhyM);
        if (!sWhy.empty()) ImGui::TextDisabled("%s", sWhy.c_str());
    }
}

}  // namespace quickstart
}  // namespace recomp
