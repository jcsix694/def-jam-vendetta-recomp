// Def Jam Vendetta (USA, SLUS-20639) - the fight camera's styles, as the pause menu sees them (vendetta_camera.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#pragma once

#include <cstdint>

namespace vendetta_camera
{
    // the styles, in the order the pause menu's CAMERA row offers them (settings.toml [graphics] camera: the names)
    enum Style : int
    {
        kVendetta = 0, // "vendetta": the game's own
        kNoMercy = 1,  // "nomercy": in the style of WWF No Mercy
        kWk2 = 2,      // "wk2": in the style of Wrestle Kingdom 2
        kStyles = 3,
    };

    // the style in use
    int style();

    // The style changed while the game runs (the pause menu): used from the next picture, the camera cut straight to it.
    // save: written into settings.toml's [graphics] camera line (the settings program reads the same line), unless the
    // game was started with VENDETTA_CAMERA set to something else than the file says (a test's switch wins) or there is
    // no settings file. Runs on the game's thread.
    void chooseStyle(uint8_t *rdram, int style, bool save);
}
