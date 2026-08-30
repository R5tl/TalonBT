// Talon — Cerbios In-Game Reset (IGR) combos.
//
// Cerbios watches the controller for button combinations and, on a match,
// resets/reloads/shuts down the console from in-game. Talon can hold any combo
// over WiFi, so it exposes those same defaults as one-tap shortcuts.
//
// Combo encoding is Cerbios's own: a 4-nibble hex string, each nibble a button
// (A=0 B=1 X=2 Y=3 BLACK=4 WHITE=5 LT=6 RT=7 UP=8 DOWN=9 LEFT=A RIGHT=B
// START=C BACK=D LTHUMB=E RTHUMB=F). Extracted from CerbiosToolInternal
// (Config.cs defaults). Unused nibble slots repeat a button harmlessly.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Trigger an IGR action by name: dash game full shutdown cycle screen.
// Presses the Cerbios combo, holds ~800 ms so the BIOS scan catches it, then
// releases everything. Returns false for an unknown action. Blocks for the
// hold, so it runs on the httpd worker task.
bool talon_igr_trigger(const char *action);

#ifdef __cplusplus
}
#endif
