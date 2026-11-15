// Def Jam Vendetta (USA, SLUS-20639) - the CAMERA row of the pause menu and of Options: the fight camera's style chosen
// in a match or on the menus (Options' part: the section "Options" below).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The pause menu ("GAME PAUSED", only in a fight) is two routines, both run once a picture while the match flags
// 0x22FDE4 have bit 1 (and not bit 3):
//   FUN_001433d8(pad, pad, pad, pad)  its input (called by FUN_0011afe8; -1 = no pad): returns 0 stay, 1 go on, 2 quit
//   FUN_00143b98()                    its drawing (called by the fight's picture FUN_0011a038)
// FUN_00142690(2) opens it. They share Options' block 0x29F448: +0x4 the row, +0x1C..+0x23 the options being changed
// (a copy of the saved 8 bytes at 0x240F84: +0 effects, +1 speech, +2 music, each 0-32; +5 vibration 0 / 1), +0x24
// the QUIT question open, +0x28 its answer (1 No, 0 Yes), +0x2C the help box open (square held).
// The game's six rows: EFFECTS, SPEECH, MUSIC (sliders), VIBRATION (OFF / ON), QUIT, CONTINUE, 28 apart from y 140.
//
// Both routines are replaced here by the same steps with a seventh row, CAMERA (VENDETTA / NO MERCY / WRESTLE KINGDOM
// 2), between MUSIC and VIBRATION; the rows below it move down one place (28). The block's +0x4 holds this file's row
// (0-6) while the pause is open. Every call into the game's code (its sounds, the volume setters, its row pieces) runs
// one at a time, each returning to an address answered here, so a call that waits never leaves this code half done.
// The camera row is a choice row as VIBRATION is (its sounds, its arrows, its value word not lit). Its words are the
// recomp's, in the store's fixed area (a menu theme names them by their addresses, config/menu_pieces.csv).
//
// The style chosen follows the game's own rule for the pause's options: kept when the pause closes with START,
// CONTINUE or QUIT, put back when it closes with triangle. Kept = used from the next picture and written into
// settings.toml (vendetta_camera.cpp). VENDETTA_PAUSE=0: the game's own pause menu (tests). VENDETTA_PAUSELOG=1: a line for each press while the pause is open; every run
// logs the pause opened and closed ([pause]).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_camera.h"
#include "vendetta_store.h"

#include <algorithm>
#include <cstdlib>
#include <bit>
#include <cstring>
#include <iostream>
#include <vector>

namespace vendetta_pause
{
    // the game's
    constexpr uint32_t kInput = 0x1433D8u;     // FUN_001433d8(pads): the pause's input
    constexpr uint32_t kDraw = 0x143B98u;      // FUN_00143b98(): the pause's drawing
    constexpr uint32_t kOpen = 0x142690u;      // FUN_00142690(kind): Options' / the pause's block set up (2 = the pause)
    constexpr uint32_t kBlock = 0x29F448u;
    constexpr uint32_t kRow = kBlock + 0x4u;
    constexpr uint32_t kValues = kBlock + 0x1Cu; // 8 bytes
    constexpr uint32_t kQuitOpen = kBlock + 0x24u;
    constexpr uint32_t kQuitAnswer = kBlock + 0x28u;
    constexpr uint32_t kHelpOpen = kBlock + 0x2Cu;
    constexpr uint32_t kSaved = 0x240F84u;     // the options' 8 bytes

    constexpr uint32_t kPauseSound = 0x1C4F28u; // FUN_001c4f28(n): the pause's own sounds (0 open, 1 close, 2 bump, 3 move, 4 change)
    constexpr uint32_t kMenuSound = 0x1C32F0u;  // FUN_001c32f0(n): the menus' sounds (VIBRATION's: 4 change, 5 bump)
    constexpr uint32_t kSetEffects = 0x1BFB10u, kSetSpeech = 0x1BFB68u, kSetMusic = 0x1BFBC0u; // (a step 0-32)
    constexpr uint32_t kMusicOff = 0x1B1278u;   // FUN_001b1278(): the song stopped (music at 0)

    constexpr uint32_t kPanel = 0x144F48u;      // FUN_00144f48(): the dark panel, its art, GAME PAUSED, square HELP
    constexpr uint32_t kFontBegin = 0x1BAA30u;  // FUN_001baa30(font) / FUN_001bb0d0(font): a block of words
    constexpr uint32_t kFontEnd = 0x1BB0D0u;
    constexpr uint32_t kFlush = 0x1A1B00u;      // FUN_001a1b00()
    constexpr uint32_t kRowEffects = 0x1448F0u, kRowSpeech = 0x1449F8u, kRowMusic = 0x144B00u; // (x, y, ctx, value, selected)
    constexpr uint32_t kRowVibration = 0x144D18u; // (x, y, ctx, value, selected)
    constexpr uint32_t kRowQuit = 0x144E80u, kRowContinue = 0x144EB0u; // (x, y, ctx, selected)
    constexpr uint32_t kWord = 0x143EF0u;       // FUN_00143ef0(x, y, selected, text, centred, dim): Options' word
    constexpr uint32_t kArrow = 0x1445C8u;      // FUN_001445c8(x, y, ctx, 1 left / 0 right): Options' arrow
    constexpr uint32_t kMeasure = 0x1BA8A0u;    // FUN_001ba8a0(font, text): a text's width, in 1/16 GS pixels
    constexpr uint32_t kQuitBox = 0x1663F8u;    // FUN_001663f8(ctx, record): the popup window (QUIT?)
    constexpr uint32_t kHelpBox = 0x177A68u;    // FUN_00177a68(ctx, record): a menu's help box
    constexpr uint32_t kFont = 0x257F00u, kCtx = 0x25814Cu;
    constexpr uint32_t kHelpTitle = 0x2956A8u;  // "Help" (Options' and the pause's)
    constexpr uint32_t kHelpLines = 0x27C6B0u;  // the pause's 4 help lines {button, text}

    constexpr uint32_t kUp = 0x1u, kRight = 0x2u, kDown = 0x4u, kLeft = 0x8u, kStart = 0x80u, kTriangle = 0x1000u,
                       kCross = 0x4000u, kSquare = 0x8000u;

    // code addresses no recompiled function covers (the top of 0x1DF760-0x1DF8EF)
    constexpr uint32_t kInputReturn = 0x1DF8E0u, kDrawReturn = 0x1DF8E4u;

    // the store's fixed area, offsets 0xE000-0xE1FF (vendetta_store.h)
    constexpr uint32_t kAreaOffset = 0xE000u, kAreaSize = 0x200u;
    constexpr uint32_t kQuitRecord = 0x00u;     // {4, 250.0, 150.0, 1.0, the answer}
    constexpr uint32_t kHelpRecord = 0x20u;     // {109, 181, 422, 213 (floats), title, 4, lines}
    constexpr uint32_t kWords = 0x40u;          // the new words, 0x20 apart, in this order: a menu theme names them by address
    enum Word { kWordCamera, kWordVendetta, kWordNoMercy, kWordWk2, kWordCount };
    const char *const kWordText[kWordCount] = {"CAMERA", "VENDETTA", "NO MERCY", "WRESTLE KINGDOM 2"};

    // the rows (this file's numbering) and their places
    enum Row { kEffects, kSpeech, kMusic, kCamera, kVibration, kQuit, kContinue, kRows };
    constexpr float kX = 65.0f, kTop = 140.0f, kStep = 28.0f;
    constexpr float kValueX = 390.0f;          // a choice row's value, centred this far right of the row's x
    constexpr float kArrowGap = 25.0f, kArrowWidth = 30.0f, kArrowDown = 2.0f; // as Options' rows (vendetta_cage_menu.cpp)

    PS2Runtime::RecompiledFunction g_originalOpen = nullptr;
    uint32_t g_area = 0;
    int g_camera = 0;        // the CAMERA row's value while the pause is open
    int g_cameraAtOpen = 0;
    int g_optRow = 0;        // Options' row, this file's numbering (with CAMERA)
    bool g_log = false;      // VENDETTA_PAUSELOG=1: a line for each press while the pause is open

    struct Call
    {
        uint32_t function;
        uint32_t a0;
    };

    struct InputRun
    {
        std::vector<Call> calls;
        size_t next = 0;
        uint32_t result = 0;
        uint32_t returnTo = 0;
    } g_input;

    // the CAMERA row being drawn (the pause's and Options' alike)
    struct CameraRow
    {
        float x = 0.0f, y = 0.0f; // the row's place (its label's)
        bool selected = false;
        int part = 0;             // the next piece
        float gap = 0.0f;         // the arrows from the value's middle
    };

    struct DrawRun
    {
        int step = 0;
        CameraRow camera;
        uint32_t returnTo = 0;
    } g_draw;

    uint32_t word(Word w) { return g_area + kWords + 0x20u * uint32_t(w); }
    uint32_t cameraWord(int style) { return word(Word(kWordVendetta + std::clamp(style, 0, 2))); }

    void callGuest(R5900Context *ctx, uint32_t returnTo, uint32_t function, uint32_t a0 = 0u, uint32_t a1 = 0u,
                   uint32_t a2 = 0u, uint32_t a3 = 0u);

    // The CAMERA row's next call into the game's code (returning to `back`): its label, its value centred, and while it
    // is the selected row an arrow on each side it can still go (the value measured in the face it was drawn in);
    // false when the row is drawn.
    bool cameraRowStep(CameraRow &r, R5900Context *ctx, uint32_t back)
    {
        const float cx = r.x + kValueX;
        for (;;)
        {
            switch (r.part++)
            {
            case 0:
                ctx->f[12] = r.x;
                ctx->f[13] = r.y;
                callGuest(ctx, back, kWord, r.selected, word(kWordCamera), 0u, 0u);
                return true;
            case 1:
                ctx->f[12] = cx;
                ctx->f[13] = r.y;
                callGuest(ctx, back, kWord, 0u, cameraWord(g_camera), 1u, 0u);
                return true;
            case 2:
                if (!r.selected)
                    return false;
                callGuest(ctx, back, kMeasure, kFont, cameraWord(g_camera));
                return true;
            case 3:
            {
                const float half = float(static_cast<int32_t>(getRegU32(ctx, 2))) / 25.6f; // 1/16 GS pixel = 1/12.8 px
                r.gap = std::max(kArrowGap, half + 5.0f);
                if (g_camera > 0)
                {
                    ctx->f[12] = cx - r.gap - kArrowWidth;
                    ctx->f[13] = r.y + kArrowDown;
                    callGuest(ctx, back, kArrow, kCtx, 1u);
                    return true;
                }
                continue;
            }
            case 4:
                if (g_camera < vendetta_camera::kStyles - 1)
                {
                    ctx->f[12] = cx + r.gap;
                    ctx->f[13] = r.y + kArrowDown;
                    callGuest(ctx, back, kArrow, kCtx, 0u);
                    return true;
                }
                return false;
            default:
                return false;
            }
        }
    }

    void callGuest(R5900Context *ctx, uint32_t returnTo, uint32_t function, uint32_t a0, uint32_t a1, uint32_t a2,
                   uint32_t a3)
    {
        SET_GPR_U32(ctx, 4, a0);
        SET_GPR_U32(ctx, 5, a1);
        SET_GPR_U32(ctx, 6, a2);
        SET_GPR_U32(ctx, 7, a3);
        SET_GPR_U32(ctx, 31, returnTo);
        ctx->pc = function;
    }

    // FUN_001aff70(pad, kind) read as it reads: 0 held, 1 pressed, 2 pressed or repeating (the directions' repeat);
    // the low byte's two halves swapped (0x01 up, 0x02 right, 0x04 down, 0x08 left, 0x80 start)
    uint32_t padBits(uint8_t *rdram, R5900Context *ctx, int32_t pad, int kind)
    {
        if (pad < 0 || pad >= 8)
            return 0u;
        if (pad == 0 && static_cast<int32_t>(Ps2FastRead32(rdram, getRegU32(ctx, 28) - 0x1C30u)) != 1)
            return 0u;
        const uint32_t base = 0x01431010u + uint32_t(pad) * 0x40u;
        if (Ps2FastRead32(rdram, base) != 0x79u)
            return 0u;
        uint32_t bits = kind == 0 ? Ps2FastRead32(rdram, base + 4u)
                      : kind == 1 ? Ps2FastRead32(rdram, base + 8u)
                                  : Ps2FastRead32(rdram, 0x01431210u + uint32_t(pad) * 4u);
        return (bits & 0xFF00u) | ((bits & 0xF0u) >> 4) | ((bits & 0xFu) << 4);
    }

    // ---- the input: FUN_001433d8's steps, with the CAMERA row

    void continueInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (g_input.next < g_input.calls.size())
        {
            const Call call = g_input.calls[g_input.next++];
            callGuest(ctx, kInputReturn, call.function, call.a0);
            return;
        }
        SET_GPR_U32(ctx, 2, g_input.result);
        ctx->pc = g_input.returnTo;
    }

    void input(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t held = 0u, pressed = 0u, repeats = 0u;
        for (int i = 0; i < 4; ++i)
        {
            const int32_t pad = static_cast<int32_t>(getRegU32(ctx, 4 + i));
            if (pad == -1)
                continue;
            held |= padBits(rdram, ctx, pad, 0);
            pressed |= padBits(rdram, ctx, pad, 1);
            repeats |= padBits(rdram, ctx, pad, 2);
        }
        g_input = InputRun{};
        g_input.returnTo = getRegU32(ctx, 31);
        auto &calls = g_input.calls;
        const auto pauseSound = [&](uint32_t n) { calls.push_back({kPauseSound, n}); };
        const auto menuSound = [&](uint32_t n) { calls.push_back({kMenuSound, n}); };
        uint8_t values[8];
        for (uint32_t i = 0; i < 8u; ++i)
            values[i] = Ps2FastRead8(rdram, kValues + i);
        int row = static_cast<int32_t>(Ps2FastRead32(rdram, kRow));
        uint32_t result = 0u;
        bool cancelled = false;

        if ((held & kSquare) == 0u)
        {
            if (Ps2FastRead32(rdram, kHelpOpen) != 0u)
            {
                Ps2FastWrite32(rdram, kHelpOpen, 0u);
                pauseSound(1u);
            }
            if (Ps2FastRead32(rdram, kQuitOpen) == 0u)
            {
                if (repeats & kUp)
                {
                    if (--row < 0)
                    {
                        row = 0;
                        pauseSound(2u);
                    }
                    else
                        pauseSound(3u);
                }
                if (repeats & kDown)
                {
                    if (++row < kRows)
                        pauseSound(3u);
                    else
                    {
                        row = kRows - 1;
                        pauseSound(2u);
                    }
                }
                switch (row)
                {
                case kEffects:
                case kSpeech:
                case kMusic:
                {
                    const int8_t before = static_cast<int8_t>(values[row]);
                    int8_t value = before;
                    if (repeats & kLeft)
                    {
                        --value;
                        if (value < 0)
                        {
                            value = 0;
                            pauseSound(2u);
                        }
                        else
                        {
                            if (row == kMusic && value == 0)
                                calls.push_back({kMusicOff, 0u});
                            pauseSound(4u);
                        }
                    }
                    if (repeats & kRight)
                    {
                        ++value;
                        if (value < 33)
                            pauseSound(4u);
                        else
                        {
                            value = 32;
                            pauseSound(2u);
                        }
                    }
                    values[row] = static_cast<uint8_t>(value);
                    if (value != before)
                        calls.push_back({row == kEffects ? kSetEffects : row == kSpeech ? kSetSpeech : kSetMusic,
                                         uint32_t(static_cast<int32_t>(value))});
                    break;
                }
                case kCamera:
                    if (repeats & kLeft)
                    {
                        if (--g_camera < 0)
                        {
                            g_camera = 0;
                            menuSound(5u);
                        }
                        else
                            menuSound(4u);
                    }
                    if (repeats & kRight)
                    {
                        if (++g_camera < vendetta_camera::kStyles)
                            menuSound(4u);
                        else
                        {
                            g_camera = vendetta_camera::kStyles - 1;
                            menuSound(5u);
                        }
                    }
                    break;
                case kVibration:
                {
                    int8_t value = static_cast<int8_t>(values[5]);
                    if (repeats & kLeft)
                    {
                        if (--value < 0)
                        {
                            value = 0;
                            menuSound(5u);
                        }
                        else
                            menuSound(4u);
                    }
                    if (repeats & kRight)
                    {
                        if (++value < 2)
                            menuSound(4u);
                        else
                        {
                            value = 1;
                            menuSound(5u);
                        }
                    }
                    values[5] = static_cast<uint8_t>(value);
                    break;
                }
                case kQuit:
                    if (pressed & kCross)
                    {
                        Ps2FastWrite32(rdram, kQuitAnswer, 1u);
                        Ps2FastWrite32(rdram, kQuitOpen, 1u);
                        pauseSound(1u);
                    }
                    break;
                case kContinue:
                    if (pressed & kCross)
                    {
                        result = 1u;
                        pauseSound(1u);
                    }
                    break;
                }
                if (pressed & kStart)
                {
                    result = 1u;
                    pauseSound(1u);
                }
                if (pressed & kTriangle)
                {
                    // put back as they were (the saved bytes, the volumes set again, the camera as it was)
                    for (uint32_t i = 0; i < 8u; ++i)
                        values[i] = Ps2FastRead8(rdram, kSaved + i);
                    pauseSound(1u);
                    result = 1u;
                    cancelled = true;
                    calls.push_back({kSetEffects, uint32_t(static_cast<int8_t>(values[0]))});
                    calls.push_back({kSetSpeech, uint32_t(static_cast<int8_t>(values[1]))});
                    calls.push_back({kSetMusic, uint32_t(static_cast<int8_t>(values[2]))});
                    if (values[2] == 0u)
                        calls.push_back({kMusicOff, 0u});
                }
            }
            else
            {
                // the QUIT question: up / down between Yes and No, cross answers, triangle closes it
                uint32_t answer = Ps2FastRead32(rdram, kQuitAnswer);
                if (repeats & kUp)
                {
                    if (answer == 1u)
                    {
                        answer = 0u;
                        pauseSound(3u);
                    }
                    else
                        pauseSound(2u);
                }
                if (repeats & kDown)
                {
                    if (answer == 0u)
                    {
                        answer = 1u;
                        pauseSound(3u);
                    }
                    else
                        pauseSound(2u);
                }
                Ps2FastWrite32(rdram, kQuitAnswer, answer);
                if (pressed & kCross)
                {
                    if (answer == 0u)
                        result = 2u;
                    else
                        Ps2FastWrite32(rdram, kQuitOpen, 0u);
                    pauseSound(0u);
                }
                else if (pressed & kTriangle)
                {
                    Ps2FastWrite32(rdram, kQuitOpen, 0u);
                    pauseSound(1u);
                }
            }
        }
        else if (Ps2FastRead32(rdram, kHelpOpen) == 0u)
        {
            Ps2FastWrite32(rdram, kHelpOpen, 1u);
            pauseSound(0u);
        }

        if (g_log && (pressed | repeats) != 0u)
            std::cerr << "[pause] pressed 0x" << std::hex << pressed << ", repeats 0x" << repeats << std::dec << ": row "
                      << row << ", camera " << g_camera << std::endl;
        Ps2FastWrite32(rdram, kRow, static_cast<uint32_t>(row));
        for (uint32_t i = 0; i < 8u; ++i)
            Ps2FastWrite8(rdram, kValues + i, values[i]);
        if (result != 0u)
        {
            for (uint32_t i = 0; i < 8u; ++i)
                Ps2FastWrite8(rdram, kSaved + i, values[i]);
            if (cancelled)
                g_camera = g_cameraAtOpen;
            std::cerr << "[pause] closed (" << (result == 2u ? "quit" : cancelled ? "triangle: put back" : "kept")
                      << "), camera " << g_camera << std::endl;
            vendetta_camera::chooseStyle(rdram, g_camera, !cancelled);
        }
        g_input.result = result;
        continueInput(rdram, ctx, runtime);
    }

    // FUN_00142690(kind): kind 2 opens the pause; the CAMERA row starts on the style in use
    void open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t kind = static_cast<int32_t>(getRegU32(ctx, 4));
        if (kind >= 0)
        {
            g_camera = g_cameraAtOpen = vendetta_camera::style();
            g_optRow = 0;
            std::cerr << "[pause] " << (kind == 2 ? "opened" : "Options opened") << ", camera " << g_camera << std::endl;
        }
        g_originalOpen(rdram, ctx, runtime);
    }

    // ---- the drawing: FUN_00143b98's steps, with the CAMERA row

    void continueDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const int row = static_cast<int32_t>(Ps2FastRead32(rdram, kRow));
        const auto y = [](int r) { return kTop + kStep * float(r); };
        const auto at = [&](float x, float yy) {
            ctx->f[12] = x;
            ctx->f[13] = yy;
        };
        for (;;)
        {
            switch (g_draw.step++)
            {
            case 0:
                callGuest(ctx, kDrawReturn, kPanel);
                return;
            case 1:
                callGuest(ctx, kDrawReturn, kFontBegin, kFont);
                return;
            case 2:
                at(kX, y(kEffects));
                callGuest(ctx, kDrawReturn, kRowEffects, kCtx, Ps2FastRead8(rdram, kValues + 0u), row == kEffects);
                return;
            case 3:
                at(kX, y(kSpeech));
                callGuest(ctx, kDrawReturn, kRowSpeech, kCtx, Ps2FastRead8(rdram, kValues + 1u), row == kSpeech);
                return;
            case 4:
                at(kX, y(kMusic));
                callGuest(ctx, kDrawReturn, kRowMusic, kCtx, Ps2FastRead8(rdram, kValues + 2u), row == kMusic);
                return;
            case 5:
                if (g_draw.camera.part == 0)
                    g_draw.camera = CameraRow{kX, y(kCamera), row == kCamera};
                if (cameraRowStep(g_draw.camera, ctx, kDrawReturn))
                {
                    --g_draw.step;
                    return;
                }
                continue;
            case 6:
                at(kX, y(kVibration));
                callGuest(ctx, kDrawReturn, kRowVibration, kCtx, Ps2FastRead8(rdram, kValues + 5u), row == kVibration);
                return;
            case 7:
                at(kX, y(kQuit));
                callGuest(ctx, kDrawReturn, kRowQuit, kCtx, row == kQuit);
                return;
            case 8:
                at(kX, y(kContinue));
                callGuest(ctx, kDrawReturn, kRowContinue, kCtx, row == kContinue);
                return;
            case 9:
                callGuest(ctx, kDrawReturn, kFontEnd, kFont);
                return;
            case 10:
                callGuest(ctx, kDrawReturn, kFlush);
                return;
            case 11:
                if (Ps2FastRead32(rdram, kQuitOpen) == 0u)
                    continue;
                Ps2FastWrite32(rdram, g_area + kQuitRecord + 0x10u, Ps2FastRead32(rdram, kQuitAnswer));
                callGuest(ctx, kDrawReturn, kQuitBox, kCtx, g_area + kQuitRecord);
                return;
            case 12:
                callGuest(ctx, kDrawReturn, kFontBegin, kFont);
                return;
            case 13:
                if (Ps2FastRead32(rdram, kHelpOpen) == 0u)
                    continue;
                callGuest(ctx, kDrawReturn, kHelpBox, kCtx, g_area + kHelpRecord);
                return;
            case 14:
                callGuest(ctx, kDrawReturn, kFontEnd, kFont);
                return;
            case 15:
                callGuest(ctx, g_draw.returnTo, kFlush); // the routine's last call: it returns to the routine's caller
                return;
            }
        }
    }

    void draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_draw = DrawRun{};
        g_draw.returnTo = getRegU32(ctx, 31);
        continueDraw(rdram, ctx, runtime);
    }

    // ---- Options (the main menu's item 5, and the story's): FUN_00142768, a loop of its own until it is left
    //
    // Its seven rows keep the game's numbers in the block's +0x4 (0 EFFECTS, 1 SPEECH, 2 MUSIC, 3 DIFFICULTY,
    // 4 VIBRATION, 5 the card's row, 6 EXIT); this file keeps its own row (g_optRow: the game's 0-3, then CAMERA, then the
    // game's 4-6) and config/patches.toml gives the routine three hooks and three heights:
    //   0x142808  jal kOptInput (was FUN_001906d8): before the input, the game's row set from ours; on CAMERA with a
    //             direction pressed the input is ours that picture (the phase +0x8 set aside, so the routine's is skipped)
    //   0x14319C  jal kOptDraw (was FUN_00171000): after the input, ours follows the game's move (a step from DIFFICULTY
    //             down or from VIBRATION up lands on CAMERA), the phase put back; for the drawing the game's row is ours
    //             (CAMERA: none of the game's rows lit)
    //   0x14325C  jal kOptCamera (was FUN_00144d18): the CAMERA row first, at VIBRATION's old place, then VIBRATION
    //   0x143248 / 0x143278 / 0x143298  VIBRATION, the card's row and EXIT one place (28) lower
    // The style is kept when Options is left with its own rows (as the options are) and put back with triangle.
    constexpr uint32_t kOptPhase = kBlock + 0x8u; // 2 = taking input, 3 leaving, 4 saving
    constexpr uint32_t kOptStatus = 0x1906D8u;    // FUN_001906d8(0): the memory card's state (the call the hook replaces)
    constexpr uint32_t kOptBackdrop = 0x171000u;  // FUN_00171000(ctx): the backdrop (the call the hook replaces)
    constexpr uint32_t kOptInput = 0x1DF8C0u, kOptInputReturn = 0x1DF8C4u, kOptDraw = 0x1DF8C8u, kOptCamera = 0x1DF8CCu,
                       kOptCameraReturn = 0x1DF8D0u;
    constexpr int kOptCameraRow = 4, kOptRows = 8; // ours: 0-3 the game's, 4 CAMERA, 5-7 the game's 4-6
    constexpr uint32_t kNoRow = 99u;               // the game's row while CAMERA is the selected one: none lit
    constexpr uint32_t kSkipPhase = 0x77u;         // the phase while our input stands in for the routine's

    uint32_t g_optGameRow = 0;     // the game's row before its input
    uint8_t g_optDifficulty = 0, g_optVibration = 0;
    bool g_optSkipped = false;
    bool g_optTaking = false;      // the phase was 2 before the input
    uint32_t g_optSound = 0;       // our input's sound, then the call the hook replaces
    uint32_t g_optReturn = 0;      // the routine, after the call the input hook replaces
    PS2Runtime::RecompiledFunction g_optStatus = nullptr, g_optBackdrop = nullptr, g_optVibrationRow = nullptr;

    uint32_t gameRowOf(int ours) { return ours < kOptCameraRow ? uint32_t(ours) : uint32_t(ours - 1); }
    int oursOf(uint32_t game) { return game < uint32_t(kOptCameraRow) ? int(game) : int(game) + 1; }

    void optStatus(R5900Context *ctx)
    {
        SET_GPR_U32(ctx, 4, 0u);
        SET_GPR_U32(ctx, 31, g_optReturn);
        ctx->pc = kOptStatus;
    }

    void optInputReturn(uint8_t *, R5900Context *ctx, PS2Runtime *) { optStatus(ctx); }

    // in place of jal FUN_001906d8 at 0x142808 ($a0 = 0 from the delay slot)
    void optInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_optReturn = getRegU32(ctx, 31);
        g_optSkipped = false;
        g_optSound = 0u;
        g_optTaking = Ps2FastRead32(rdram, kOptPhase) == 2u;
        if (g_optTaking)
        {
            const uint32_t repeats = padBits(rdram, ctx, 0, 2);
            if (g_optRow == kOptCameraRow && (repeats & (kUp | kDown | kLeft | kRight)) != 0u)
            {
                // CAMERA's own input this picture: the routine's skipped
                if (repeats & kUp)
                {
                    g_optRow = kOptCameraRow - 1;
                    g_optSound = 3u;
                }
                else if (repeats & kDown)
                {
                    g_optRow = kOptCameraRow + 1;
                    g_optSound = 3u;
                }
                else if (repeats & kLeft)
                {
                    g_optSound = g_camera > 0 ? 4u : 5u;
                    g_camera = std::max(g_camera - 1, 0);
                }
                else
                {
                    g_optSound = g_camera < vendetta_camera::kStyles - 1 ? 4u : 5u;
                    g_camera = std::min(g_camera + 1, vendetta_camera::kStyles - 1);
                }
                Ps2FastWrite32(rdram, kOptPhase, kSkipPhase);
                g_optSkipped = true;
            }
            g_optGameRow = g_optRow == kOptCameraRow ? kNoRow : gameRowOf(g_optRow);
            Ps2FastWrite32(rdram, kRow, g_optGameRow);
            g_optDifficulty = Ps2FastRead8(rdram, kValues + 3u);
            g_optVibration = Ps2FastRead8(rdram, kValues + 5u);
            if (g_log && g_optSkipped)
                std::cerr << "[pause] Options: repeats 0x" << std::hex << repeats << std::dec << ": row " << g_optRow
                          << ", camera " << g_camera << std::endl;
        }
        if (g_optSound != 0u)
        {
            callGuest(ctx, kOptInputReturn, kMenuSound, g_optSound);
            return;
        }
        optStatus(ctx);
    }

    // in place of jal FUN_00171000 at 0x14319C ($a0 the context): after the input, before the drawing
    void optDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_optSkipped)
            Ps2FastWrite32(rdram, kOptPhase, 2u);
        else if (g_optTaking)
        {
            const uint32_t now = Ps2FastRead32(rdram, kRow);
            if (g_optGameRow == kNoRow)
            {
                if (now != kNoRow) // (no direction was pressed: the game's row can't have moved)
                    g_optRow = oursOf(now);
            }
            else if (g_optGameRow == 3u && now == 4u)
            {
                g_optRow = kOptCameraRow; // down from DIFFICULTY: CAMERA (VIBRATION as it was)
                Ps2FastWrite8(rdram, kValues + 5u, g_optVibration);
            }
            else if (g_optGameRow == 4u && now == 3u)
            {
                g_optRow = kOptCameraRow; // up from VIBRATION: CAMERA (DIFFICULTY as it was)
                Ps2FastWrite8(rdram, kValues + 3u, g_optDifficulty);
            }
            else
                g_optRow = oursOf(now);
            // left: triangle puts the camera back, the rows keep it (as the options themselves)
            if (Ps2FastRead32(rdram, kOptPhase) == 3u)
            {
                const bool cancelled = (padBits(rdram, ctx, 0, 1) & kTriangle) != 0u;
                if (cancelled)
                    g_camera = g_cameraAtOpen;
                std::cerr << "[pause] Options left (" << (cancelled ? "triangle: put back" : "kept") << "), camera "
                          << g_camera << std::endl;
                vendetta_camera::chooseStyle(rdram, g_camera, !cancelled);
            }
        }
        Ps2FastWrite32(rdram, kRow, g_optRow == kOptCameraRow ? kNoRow : gameRowOf(g_optRow));
        g_optBackdrop(rdram, ctx, runtime);
    }

    // in place of jal FUN_00144d18 at 0x14325C (VIBRATION, its y already one place lower): CAMERA first
    struct OptRow
    {
        CameraRow camera;
        float f12 = 0.0f, f13 = 0.0f;
        uint32_t a0 = 0, a1 = 0, a2 = 0, returnTo = 0;
    } g_optDrawRow;

    void optCameraReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cameraRowStep(g_optDrawRow.camera, ctx, kOptCameraReturn))
            return;
        ctx->f[12] = g_optDrawRow.f12;
        ctx->f[13] = g_optDrawRow.f13;
        SET_GPR_U32(ctx, 4, g_optDrawRow.a0);
        SET_GPR_U32(ctx, 5, g_optDrawRow.a1);
        SET_GPR_U32(ctx, 6, g_optDrawRow.a2);
        SET_GPR_U32(ctx, 31, g_optDrawRow.returnTo);
        g_optVibrationRow(rdram, ctx, runtime);
    }

    void optCamera(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto &r = g_optDrawRow;
        r.f12 = ctx->f[12];
        r.f13 = ctx->f[13];
        r.a0 = getRegU32(ctx, 4);
        r.a1 = getRegU32(ctx, 5);
        r.a2 = getRegU32(ctx, 6);
        r.returnTo = getRegU32(ctx, 31);
        r.camera = CameraRow{r.f12, r.f13 - kStep, g_optRow == kOptCameraRow};
        optCameraReturn(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        const char *log = std::getenv("VENDETTA_PAUSELOG");
        g_log = log && *log && log[0] != '0';
        g_originalOpen = runtime.lookupFunction(kOpen);
        if (!g_originalOpen || !runtime.lookupFunction(kInput) || !runtime.lookupFunction(kDraw))
        {
            std::cerr << "[pause] the pause menu's routines aren't where the original program has them: no CAMERA row"
                      << std::endl;
            return;
        }
        g_area = vendetta_store::fixed(kAreaOffset, kAreaSize, "the pause menu's CAMERA row");
        if (!g_area)
            return;
        uint8_t *area = vendetta_store::host(g_area);
        std::memset(area, 0, kAreaSize);
        const auto put32 = [&](uint32_t offset, uint32_t value) { std::memcpy(area + offset, &value, 4); };
        const auto putF = [&](uint32_t offset, float value) { put32(offset, std::bit_cast<uint32_t>(value)); };
        put32(kQuitRecord + 0x0u, 4u);
        putF(kQuitRecord + 0x4u, 250.0f);
        putF(kQuitRecord + 0x8u, 150.0f);
        putF(kQuitRecord + 0xCu, 1.0f);
        putF(kHelpRecord + 0x0u, 109.0f);
        putF(kHelpRecord + 0x4u, 181.0f);
        putF(kHelpRecord + 0x8u, 422.0f);
        putF(kHelpRecord + 0xCu, 213.0f);
        put32(kHelpRecord + 0x10u, kHelpTitle);
        put32(kHelpRecord + 0x14u, 4u);
        put32(kHelpRecord + 0x18u, kHelpLines);
        for (int w = 0; w < kWordCount; ++w)
            std::memcpy(area + kWords + 0x20u * uint32_t(w), kWordText[w], std::strlen(kWordText[w]) + 1u);

        g_optStatus = runtime.lookupFunction(kOptStatus);
        g_optBackdrop = runtime.lookupFunction(kOptBackdrop);
        g_optVibrationRow = runtime.lookupFunction(kRowVibration);
        // Options' patched calls (config/patches.toml) have only these answers: always registered
        runtime.registerFunction(kOptInput, optInput);
        runtime.registerFunction(kOptInputReturn, optInputReturn);
        runtime.registerFunction(kOptDraw, optDraw);
        runtime.registerFunction(kOptCamera, optCamera);
        runtime.registerFunction(kOptCameraReturn, optCameraReturn);
        runtime.registerFunction(kInputReturn, continueInput);
        runtime.registerFunction(kDrawReturn, continueDraw);
        runtime.replaceFunction(kOpen, open);
        const char *on = std::getenv("VENDETTA_PAUSE");
        if (on && on[0] == '0')
        {
            std::cerr << "[pause] VENDETTA_PAUSE=0: the game's own pause menu (no CAMERA row)" << std::endl;
            return;
        }
        runtime.replaceFunction(kInput, input);
        runtime.replaceFunction(kDraw, draw);
    }
}

void installVendettaPause(PS2Runtime &runtime) { vendetta_pause::install(runtime); }
