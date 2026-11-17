#pragma once
// Quick Start (F1 > Enhancements > General; docs/FEATURES.md "Quick Start"): the game's own start-up
// (wrist-strap screen, title, Rover's "Who are you?", "Preparing saved data") runs unseen and at
// full speed with the port pressing the buttons, and the player sees the town as the chosen
// resident steps out of the house. See cf_quick_start.cpp.
#include "recomp/ctx.h"

namespace recomp::wiimote {
struct State;
}

namespace recomp::quickstart {

// Guest thread, after OSInit: decides whether this launch starts quickly (setting / CF_QUICK_START,
// the save checks, the bypass key) and logs why not.
void onBoot(Ctx& c);
// Game thread, at the end of every guest frame: drives the start-up. -1: Quick Start is not running
// (the caller decides as usual); 1: hide this frame; 0: show it (an empty frame, so the window
// stays responsive).
int onGuestFrame(Ctx& c, u32 frame);
bool running();
// Host thread: the guest may run during the start-up shader hold (Quick Start wanted and not yet
// decided against, or running).
bool runDuringShaderHold();
// mWipeFader_c::draw (cf_hud.cpp hook): in the town, the circle starting to open ends Quick Start.
void noteWipeDraw(u32 fader);
double retraceSpeed();    // > 1 while running (unpaced, like fast-forward)
double ioLatencyScale();  // < 1 while running
void filterRemote(int chan, wiimote::State& st);
// Host thread (ImGui frame): a black screen while running (the empty frames shown meanwhile).
void drawOverlay();
// F1 > Enhancements > General.
void menuSection(bool& dirty);

}  // namespace recomp::quickstart
