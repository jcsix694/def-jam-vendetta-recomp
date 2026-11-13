// Def Jam Vendetta (USA, SLUS-20639) - the cage match's menu: Choose Match's second step (MATCH TYPE, NORMAL or CAGE
// MATCH, and a cage match's ways to win), and the choice put into the match's rule word at Battle's match set-up.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Choose Match is one routine, FUN_0014da00(front end), run once a picture: input (left / right move the battle type
// +0x10, cross goes on to User Setup, triangle back to the Main Menu, square held shows the help box), then drawing (the
// backdrop, the four mode icons, the battle type's big picture, its three lines of words, the help box, the header).
// The second step is the same routine with the same screen number: this file runs it before the routine, and
// config/patches.toml gives the routine four places to change:
//   0x14DEC8  the mode icon's call goes through kHookIcon: in the second step no icons are drawn
//   0x14E10C  the words' block's end goes through kHookRows: in the second step the rows are drawn there first (after the
//             big picture and the battle type's name, before the help box, which covers them), with Options' own
//             pieces FUN_00143ef0 (a word) and FUN_001445c8 (an arrow), then the font is set back as the routine had it
//   0x14DF98  the words come from a copy in the store's fixed area (kTexts): in the second step only each type's first
//             line, its name
//   0x14E12C  the help box's lines come from a copy (kHelp): in the second step Highlight / Change Options, Cancel /
//             Exit, Select Match Type (three lines the game already has)
// Each hook stands in for the call it replaces and makes exactly that call in the first step.
//
// The second step's input is read here: up / down move between the rows (the routine reads neither), left / right
// change a row's value, triangle goes back to the battle types; each with the menus' sounds (FUN_001c32f0: 4 move, 5
// bump, 2 back, 6 select). Cross is left to the routine: its own code then does everything it does today (the front end
// block's writes, the memory card's user scan, the slide into User Setup). User Setup's Back (FUN_0014cc48, 0xB -> 7)
// brings it back to the second step. Calls into the game's code run one at a time, each returning to an address
// answered here, so a call that waits never leaves this code half done.
//
// The choice is kept while the game runs (a rematch and the next Battle start with it). At Battle's match set-up
// (FUN_00140298) a cage match's choice goes into the rule word (vendetta_cage.h); NORMAL leaves the game's 0x5F as it
// is. Each match's start (FUN_00119d70) logs the rule word, whichever mode set it up.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_store.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace vendetta_cage_menu
{
    // the game's routines
    constexpr uint32_t kChooseMatch = 0x14DA00u; // FUN_0014da00(front end): Choose Match, screen 7
    constexpr uint32_t kBackFrom = 0x14CC48u;    // FUN_0014cc48(screen): the screen Back goes to (User Setup 0xB -> 7)
    constexpr uint32_t kMatchSetUp = 0x140298u;  // FUN_00140298(front end, match): Battle's match set-up, screen 0x19
    constexpr uint32_t kMatchInit = 0x119D70u;   // FUN_00119d70(rematches): every match's start, a rematch's too
    constexpr uint32_t kIcon = 0x179938u;        // FUN_00179938(ctx, {x, y, alpha, id, lit, timer}): a mode icon
    constexpr uint32_t kFontEnd = 0x1BB0D0u;     // FUN_001bb0d0(font): a block of words ends
    constexpr uint32_t kWord = 0x143EF0u;        // FUN_00143ef0(x, y, selected, text, centred, dim): Options' word
    constexpr uint32_t kArrow = 0x1445C8u;       // FUN_001445c8(x, y, ctx, 1 left / 0 right): Options' arrow
    constexpr uint32_t kMeasure = 0x1BA8A0u;     // FUN_001ba8a0(font, text): a text's width, in 1/16 GS pixels
    constexpr uint32_t kSetFace = 0x1BA720u;     // FUN_001ba720(font, face)
    constexpr uint32_t kSetScale = 0x1BA748u;    // FUN_001ba748(x scale, y scale, font, 0x80, 0)
    constexpr uint32_t kSetColour = 0x1BA7D8u;   // FUN_001ba7d8(font, r, g, b, a)
    constexpr uint32_t kSound = 0x1C32F0u;       // FUN_001c32f0(n): a menu sound

    constexpr uint32_t kFont = 0x257F00u;           // the menus' font block
    constexpr uint32_t kChooseMatchFace = 0x1017D00u; // Choose Match's words: this face at the scale 0x294D30 / 0x294D34
    constexpr uint32_t kChooseMatchScale = 0x294D30u;
    constexpr uint32_t kGameTexts = 0x2399B8u;     // 4 battle types x 3 text pointers (name, two lines)
    constexpr uint32_t kGameHelp = 0x27CCF0u;      // 3 x {s32 button, text}: Highlight Match Type, Exit, Select Match Type
    constexpr uint32_t kOff = 0x2956D8u, kOn = 0x2956E0u; // Options' OFF and ON (its table {OFF, ON} at 0x2956E8)
    constexpr uint32_t kHelpHighlight = 0x27C5F8u; // "Highlight / Change Options" (Options' help box)
    constexpr uint32_t kHelpCancel = 0x27CD28u;    // "Cancel / Exit" (Choose Stage's)
    constexpr uint32_t kHelpSelect = 0x27CCD8u;    // "Select Match Type" (Choose Match's own)
    constexpr uint32_t kButtonPad = 0u, kButtonTriangle = 1u, kButtonCross = 2u; // the help box's button pictures

    constexpr uint32_t kMatch = 0x2361C8u; // the match block: +0 the mode (1 Battle, 2 Story, 3 Survival), +4 the rule
                                           // halfword, +6 the kind byte (Battle: 1 one on one, 2 tag, 8 / 4 Free For
                                           // All, 0x20 / 0x10 handicap)

    // the front end block's fields
    constexpr uint32_t kKind = 0x10u, kPhase = 0x60u, kSlide = 0x64u, kHeld = 0xC4u, kPressed = 0xC8u, kRepeats = 0xCCu,
                       kDrawCtx = 0x134u, kBusy = 0x8B4u;
    constexpr uint32_t kUp = 0x1u, kRight = 0x2u, kDown = 0x4u, kLeft = 0x8u, kTriangle = 0x1000u, kCross = 0x4000u,
                       kSquare = 0x8000u;
    constexpr uint32_t kSoundBack = 2u, kSoundMove = 4u, kSoundBump = 5u, kSoundSelect = 6u;
    constexpr uint32_t kSetUpScreen = 0xBu, kChooseMatchScreen = 7u;

    // code addresses no recompiled function covers, 0x1DF760-0x1DF8EF (100; the ring's hooks are the range below it)
    constexpr uint32_t kHookFirst = 0x1DF760u;
    constexpr uint32_t kHookIcon = kHookFirst;          // jal from 0x14DEC8, in place of jal FUN_00179938
    constexpr uint32_t kHookRows = kHookFirst + 4u;     // jal from 0x14E10C, in place of jal FUN_001bb0d0
    constexpr uint32_t kSoundReturn = kHookFirst + 8u;  // the sound's call returns here, then the routine runs
    constexpr uint32_t kRowsReturn = kHookFirst + 12u;  // the rows' calls return here

    // the store's fixed area, offsets 0x200-0x3FF (vendetta_store.h)
    constexpr uint32_t kAreaOffset = 0x200u, kAreaSize = 0x200u;
    constexpr uint32_t kTexts = 0x00u;   // 4 x 3 text pointers (the routine's lui / addiu at 0x14DF98 / 0x14DF9C)
    constexpr uint32_t kHelp = 0x40u;    // 3 x {button, text} (the lui / addiu at 0x14E12C / 0x14E138)
    constexpr uint32_t kStrings = 0x60u; // the new words, in this order: a menu theme will name them by their address

    // the second step's rows (from the game's numbers): the battle type's name in yellow at y 300 (the routine's first
    // line), the rows from y 328, 24 apart, each label at x 136 and its value centred on x 431: the widest row (MATCH
    // TYPE ... CAGE MATCH, 136 to 504 in the game's font) sits evenly about the screen's middle, 320, and the longest
    // label, PIN & SUBMISSION, ends clear of its row's left arrow; an arrow only on the side a value can still go, as
    // Options' rows (FUN_00144d18: 25 from the middle of a short value, each arrow 30 wide)
    constexpr float kRowsTop = 328.0f, kRowStep = 24.0f, kLabelX = 136.0f, kValueX = 431.0f;
    constexpr float kArrowGap = 25.0f, kArrowWidth = 30.0f, kArrowDown = 2.0f;

    enum Row { kMatchType, kEscape, kPinSubmission, kTko, kRopeBreaks, kRows };
    enum Word { kWordMatchType, kWordNormal, kWordCage, kWordEscape, kWordPinSubmission, kWordTko, kWordRopeBreaks,
                kWordEmpty, kWords };
    const char *const kWordText[kWords] = {"MATCH TYPE", "NORMAL", "CAGE MATCH", "ESCAPE", "PIN & SUBMISSION", "TKO",
                                           "ROPE BREAKS", ""};
    const Word kRowLabel[kRows] = {kWordMatchType, kWordEscape, kWordPinSubmission, kWordTko, kWordRopeBreaks};

    PS2Runtime::RecompiledFunction g_original = nullptr, g_originalBackFrom = nullptr, g_originalSetUp = nullptr,
                                   g_originalInit = nullptr, g_icon = nullptr, g_fontEnd = nullptr;

    vendetta_cage::Rules g_choice{}; // the menu's, kept while the game runs
    vendetta_cage::Rules g_test{};   // the test match's, for its own match set-up
    bool g_haveTest = false;

    int g_stage = 1;                 // 1 the battle types, 2 the match type
    int g_row = kMatchType;
    bool g_backFromSetUp = false;    // User Setup's Back: Choose Match opens on the second step
    uint32_t g_lastPhase = 0xFFFFFFFFu;
    uint32_t g_swallow = 0;          // a button that changed the step: ignored until let go
    uint32_t g_menu = 0;             // the front end block of the routine's call
    uint32_t g_menuReturn = 0;       // ... and where it returns to

    uint32_t g_area = 0;
    bool g_filled = false;
    uint32_t g_word[kWords]{};
    uint8_t g_gameTexts[48]{}, g_secondTexts[48]{};
    uint8_t g_gameHelp[24]{}, g_secondHelp[24]{};

    struct RowsDraw
    {
        int row = 0;
        int step = 0;
        float gap = 0.0f;      // the selected value's arrows from its middle
        uint32_t returnTo = 0; // the routine, after its jal FUN_001bb0d0
    } g_rows;

    float readF(uint8_t *rdram, uint32_t a) { return std::bit_cast<float>(Ps2FastRead32(rdram, a)); }

    void callGuest(R5900Context *ctx, uint32_t returnTo, uint32_t function, uint32_t a0 = 0u, uint32_t a1 = 0u,
                   uint32_t a2 = 0u, uint32_t a3 = 0u, uint32_t t0 = 0u)
    {
        SET_GPR_U32(ctx, 4, a0);
        SET_GPR_U32(ctx, 5, a1);
        SET_GPR_U32(ctx, 6, a2);
        SET_GPR_U32(ctx, 7, a3);
        SET_GPR_U32(ctx, 8, t0);
        SET_GPR_U32(ctx, 31, returnTo);
        ctx->pc = function;
    }

    // ---- the choice

    bool *value(int row)
    {
        switch (row)
        {
        case kEscape: return &g_choice.escape;
        case kPinSubmission: return &g_choice.pinSubmission;
        case kTko: return &g_choice.tko;
        case kRopeBreaks: return &g_choice.ropeBreaks;
        default: return nullptr;
        }
    }

    int rowCount() { return g_choice.cage ? kRows : 1; }
    // rope breaks only count while pins and holds do: without them the row is dimmed, skipped and reads OFF, and the
    // match has none (Rules::ropeBreaksOn); the player's own choice is kept for when PIN & SUBMISSION is on again
    bool dim(int row) { return row == kRopeBreaks && !g_choice.pinSubmission; }
    int waysOn() { return int(g_choice.escape) + int(g_choice.pinSubmission) + int(g_choice.tko); }

    // a row's value can go left (to NORMAL, to OFF) or right (to CAGE MATCH, to ON); the last way to win that is on
    // can't go off, so no match can be set up that nobody can win
    bool canLeft(int row)
    {
        if (row == kMatchType)
            return g_choice.cage;
        const bool on = *value(row);
        return on && (row == kRopeBreaks || waysOn() > 1);
    }
    bool canRight(int row) { return row == kMatchType ? !g_choice.cage : !*value(row); }

    uint32_t valueText(int row)
    {
        if (row == kMatchType)
            return g_word[g_choice.cage ? kWordCage : kWordNormal];
        if (row == kRopeBreaks)
            return g_choice.ropeBreaksOn() ? kOn : kOff; // what the match will do: OFF while the row is dimmed
        return *value(row) ? kOn : kOff;
    }

    std::string describe(const vendetta_cage::Rules &r)
    {
        if (!r.cage)
            return "NORMAL";
        auto onOff = [](bool b) { return b ? "on" : "off"; };
        return std::string("CAGE MATCH (escape ") + onOff(r.escape) + ", pin & submission " + onOff(r.pinSubmission) +
               ", TKO " + onOff(r.tko) + ", rope breaks " + onOff(r.ropeBreaksOn()) + ")";
    }

    // ---- the tables

    uint32_t putString(uint8_t *rdram, uint32_t &top, const char *text)
    {
        const uint32_t length = static_cast<uint32_t>(std::strlen(text)) + 1u;
        const uint32_t where = g_area + top;
        std::memcpy(vendetta_store::host(where), text, length);
        top += length;
        return where;
    }

    void fill(uint8_t *rdram)
    {
        g_filled = true;
        std::memset(vendetta_store::host(g_area), 0, kAreaSize);
        uint32_t top = kStrings;
        for (int w = 0; w < kWords; ++w)
            g_word[w] = putString(rdram, top, kWordText[w]);

        std::memcpy(g_gameTexts, vendetta_store::host(kGameTexts), sizeof(g_gameTexts));
        std::memcpy(g_secondTexts, g_gameTexts, sizeof(g_secondTexts));
        for (uint32_t kind = 0; kind < 4u; ++kind) // the second step: each type's name, no lines under it
            for (uint32_t line = 1; line < 3u; ++line)
                std::memcpy(g_secondTexts + (kind * 3u + line) * 4u, &g_word[kWordEmpty], 4u);

        std::memcpy(g_gameHelp, vendetta_store::host(kGameHelp), sizeof(g_gameHelp));
        const uint32_t second[6] = {kButtonPad, kHelpHighlight, kButtonTriangle, kHelpCancel, kButtonCross, kHelpSelect};
        std::memcpy(g_secondHelp, second, sizeof(g_secondHelp));
    }

    void useTables()
    {
        std::memcpy(vendetta_store::host(g_area + kTexts), g_stage == 2 ? g_secondTexts : g_gameTexts, sizeof(g_gameTexts));
        std::memcpy(vendetta_store::host(g_area + kHelp), g_stage == 2 ? g_secondHelp : g_gameHelp, sizeof(g_gameHelp));
    }

    // ---- input

    int nextRow(int from, int step)
    {
        for (int r = from + step; r >= 0 && r < rowCount(); r += step)
            if (!dim(r))
                return r;
        return -1;
    }

    // the second step's presses (the routine gets none of them but cross); the sound to play
    uint32_t secondStep(uint8_t *rdram, uint32_t menu, uint32_t pressed, uint32_t repeats)
    {
        Ps2FastWrite32(rdram, menu + kRepeats, repeats & ~(kLeft | kRight)); // the battle type never moves here
        if (pressed & kTriangle)
        {
            Ps2FastWrite32(rdram, menu + kPressed, pressed & ~kTriangle);
            g_stage = 1;
            g_swallow |= kTriangle;
            return kSoundBack;
        }
        if (pressed & kCross)
        {
            // the routine's own cross: the front end block's writes, the card scan, User Setup
            std::cerr << "[cage] Choose Match: " << describe(g_choice) << std::endl;
            return 0u;
        }
        if (repeats & (kUp | kDown))
        {
            const int to = nextRow(g_row, (repeats & kUp) ? -1 : 1);
            if (to < 0)
                return kSoundBump;
            g_row = to;
            return kSoundMove;
        }
        if (repeats & (kLeft | kRight))
        {
            const bool right = (repeats & kRight) != 0;
            if (right ? !canRight(g_row) : !canLeft(g_row))
                return kSoundBump;
            if (g_row == kMatchType)
                g_choice.cage = right;
            else
                *value(g_row) = right;
            return kSoundMove;
        }
        return 0u;
    }

    // ---- Choose Match

    void runRoutine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SET_GPR_U32(ctx, 4, g_menu);
        SET_GPR_U32(ctx, 31, g_menuReturn);
        g_original(rdram, ctx, runtime);
    }

    // the sound's call returns here: then the routine, as if it had been called straight away
    void afterSound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { runRoutine(rdram, ctx, runtime); }

    void chooseMatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t menu = getRegU32(ctx, 4);
        g_menu = menu;
        g_menuReturn = getRegU32(ctx, 31);
        if (!g_filled)
            fill(rdram);

        // a new visit: the slide in starts (phase 0 / 1) after the screen was elsewhere
        const uint32_t phase = Ps2FastRead32(rdram, menu + kPhase);
        if (phase <= 1u && g_lastPhase > 1u)
        {
            g_stage = g_backFromSetUp ? 2 : 1;
            g_backFromSetUp = false;
            g_swallow = 0;
            if (g_row >= rowCount() || dim(g_row))
                g_row = kMatchType;
        }
        g_lastPhase = phase;

        uint32_t sound = 0;
        if (phase == 2u)
        {
            const uint32_t held = Ps2FastRead32(rdram, menu + kHeld);
            uint32_t pressed = Ps2FastRead32(rdram, menu + kPressed);
            // a press can show on more than one of the menu's pictures: the button that changed the step counts once,
            // until a picture where it is neither pressed nor held
            g_swallow &= (pressed | held);
            if (g_swallow & pressed)
            {
                pressed &= ~g_swallow;
                Ps2FastWrite32(rdram, menu + kPressed, pressed);
            }
            const uint32_t repeats = Ps2FastRead32(rdram, menu + kRepeats);
            // input unless square is held (the help box), as the routine itself reads it
            const bool taking = !(held & kSquare) || Ps2FastRead32(rdram, menu + kBusy) != 0u;
            if (taking && g_stage == 1)
            {
                if ((pressed & kCross) && Ps2FastRead32(rdram, menu + kKind) < 4u)
                {
                    // on to the second step: the routine doesn't see this cross (it would go on to User Setup), nor a
                    // left / right in the same picture
                    Ps2FastWrite32(rdram, menu + kPressed, pressed & ~kCross);
                    Ps2FastWrite32(rdram, menu + kRepeats, repeats & ~(kLeft | kRight));
                    g_stage = 2;
                    g_row = kMatchType;
                    g_swallow |= kCross;
                    sound = kSoundSelect;
                }
            }
            else if (taking)
                sound = secondStep(rdram, menu, pressed, repeats);
        }
        useTables();

        if (sound == 0u)
        {
            g_original(rdram, ctx, runtime);
            return;
        }
        callGuest(ctx, kSoundReturn, kSound, sound); // then the routine (afterSound)
    }

    // in place of the routine's jal FUN_00179938 at 0x14DEC8: no icons in the second step
    void icon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_stage != 2)
        {
            g_icon(rdram, ctx, runtime);
            return;
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    // the rows, one call into the game's code at a time; each returns here
    void continueRows(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float slide = readF(rdram, g_menu + kSlide); // the slide in / out moves every piece across
        for (;;)
        {
            if (g_rows.row >= rowCount())
            {
                // the face, scale and colour the routine's words left, for the help box and the header after them
                switch (g_rows.step++)
                {
                case 0:
                    callGuest(ctx, kRowsReturn, kSetFace, kFont, kChooseMatchFace);
                    return;
                case 1:
                    ctx->f[12] = readF(rdram, kChooseMatchScale);
                    ctx->f[13] = readF(rdram, kChooseMatchScale + 4u);
                    callGuest(ctx, kRowsReturn, kSetScale, kFont, 0x80u, 0u);
                    return;
                case 2:
                    callGuest(ctx, kRowsReturn, kSetColour, kFont, 0x80u, 0x80u, 0x80u, 0x80u);
                    return;
                default:
                    callGuest(ctx, g_rows.returnTo, kFontEnd, kFont); // the call the hook stands in for
                    return;
                }
            }
            const int row = g_rows.row;
            const bool selected = row == g_row, faded = dim(row);
            const float y = kRowsTop + kRowStep * float(row);
            const int step = g_rows.step++;
            if (step == 0)
            {
                ctx->f[12] = kLabelX + slide;
                ctx->f[13] = y;
                callGuest(ctx, kRowsReturn, kWord, selected, g_word[kRowLabel[row]], 0u, faded);
                return;
            }
            if (step == 1)
            {
                ctx->f[12] = kValueX + slide;
                ctx->f[13] = y;
                callGuest(ctx, kRowsReturn, kWord, selected, valueText(row), 1u, faded);
                return;
            }
            if (selected && !faded)
            {
                if (step == 2)
                {
                    callGuest(ctx, kRowsReturn, kMeasure, kFont, valueText(row)); // in the face the value was drawn in
                    return;
                }
                if (step == 3)
                {
                    const float half = float(static_cast<int32_t>(getRegU32(ctx, 2))) / 25.6f; // 1/16 GS pixel = 1/12.8 px
                    g_rows.gap = std::max(kArrowGap, half + 5.0f);
                    if (canLeft(row))
                    {
                        ctx->f[12] = kValueX + slide - g_rows.gap - kArrowWidth;
                        ctx->f[13] = y + kArrowDown;
                        callGuest(ctx, kRowsReturn, kArrow, Ps2FastRead32(rdram, g_menu + kDrawCtx), 1u);
                        return;
                    }
                    continue;
                }
                if (step == 4 && canRight(row))
                {
                    ctx->f[12] = kValueX + slide + g_rows.gap;
                    ctx->f[13] = y + kArrowDown;
                    callGuest(ctx, kRowsReturn, kArrow, Ps2FastRead32(rdram, g_menu + kDrawCtx), 0u);
                    return;
                }
            }
            ++g_rows.row;
            g_rows.step = 0;
        }
    }

    // in place of the routine's jal FUN_001bb0d0 at 0x14E10C ($a0 the font): the rows first in the second step
    void rows(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_stage != 2)
        {
            g_fontEnd(rdram, ctx, runtime);
            return;
        }
        g_rows = RowsDraw{};
        g_rows.returnTo = getRegU32(ctx, 31);
        continueRows(rdram, ctx, runtime);
    }

    // FUN_0014cc48(screen): User Setup's ways back (Back on a card, a memory card's message) all ask it; from Battle's
    // User Setup the answer is Choose Match, which then opens on the second step
    void backFrom(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) == kSetUpScreen)
            g_backFromSetUp = true;
        g_originalBackFrom(rdram, ctx, runtime);
    }

    // ---- the match

    std::string hex(uint32_t value, int digits)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%0*X", digits, value);
        return text;
    }

    std::string ruleNames(uint16_t word)
    {
        std::string out;
        auto add = [&](bool on, const char *name) {
            if (on)
                out += (out.empty() ? "" : ", ") + std::string(name);
        };
        add(word & vendetta_cage::kRuleCage, "cage");
        add(word & vendetta_cage::kRuleEscape, "escape");
        add(word & vendetta_cage::kRuleKO, "KOs");
        add(word & vendetta_cage::kRuleSubmission, "submissions");
        add(word & vendetta_cage::kRulePin, "pins");
        add(word & vendetta_cage::kRuleRopeBreaks, "rope breaks");
        add(word & vendetta_cage::kRuleTagBothIn, "tag partners both in");
        return out.empty() ? "none" : out;
    }

    // FUN_00140298 (Battle's match set-up, after Choose Stage; it never waits): a cage match's rules into the rule word,
    // which the routine has just written (0x5F)
    void matchSetUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const bool test = g_haveTest;
        const vendetta_cage::Rules rules = test ? g_test : g_choice;
        g_haveTest = false;
        g_originalSetUp(rdram, ctx, runtime);
        if (ctx->pc != ra)
        {
            std::cerr << "[cage] Battle's match set-up didn't return at once: the match is played by the game's rules"
                      << std::endl;
            return;
        }
        const uint16_t before = Ps2FastRead16(rdram, kMatch + 4u);
        const uint8_t kind = Ps2FastRead8(rdram, kMatch + 6u);
        uint16_t word = before;
        if (rules.cage)
        {
            using namespace vendetta_cage;
            if (!rules.tko)
                word &= ~kRuleKO;
            if (!rules.pinSubmission)
                word &= ~(kRuleSubmission | kRulePin);
            if (!rules.ropeBreaksOn()) // off by choice, or because pins and submissions are off
                word &= ~kRuleRopeBreaks;
            if (kind == 2u) // tag: both partners in the ring, no tags (as Story's tag fights)
                word |= kRuleTagBothIn;
            word |= kRuleCage;
            if (rules.escape)
                word |= kRuleEscape;
            Ps2FastWrite16(rdram, kMatch + 4u, word);
        }
        std::cerr << "[cage] Battle's match set-up" << (test ? " (the test match)" : "") << ": " << describe(rules)
                  << ", kind " << hex(kind, 2) << ", rule word " << hex(before, 4);
        if (word != before)
            std::cerr << " -> " << hex(word, 4);
        std::cerr << std::endl;
    }

    // FUN_00119d70 (every match's and rematch's start, whichever mode set it up): the rule word in the log
    void matchInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mode = Ps2FastRead32(rdram, kMatch);
        const uint16_t word = Ps2FastRead16(rdram, kMatch + 4u);
        const char *modeName = mode == 1u ? "Battle" : mode == 2u ? "Story" : mode == 3u ? "Survival" : "?";
        std::cerr << "[cage] match start (rematch " << getRegU32(ctx, 4) << "): mode " << mode << " " << modeName
                  << ", kind " << hex(Ps2FastRead8(rdram, kMatch + 6u), 2) << ", rule word " << hex(word, 4) << " ("
                  << ruleNames(word) << ")" << std::endl;
        vendetta_cage::matchStarting();
        vendetta_cage::prepareDraw(rdram, *runtime);
        vendetta_cage::climbMatchStarting(rdram, *runtime);
        g_originalInit(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        // the routine's patched calls (config/patches.toml) have only these answers: they are always registered
        g_icon = runtime.lookupFunction(kIcon);
        g_fontEnd = runtime.lookupFunction(kFontEnd);
        runtime.registerFunction(kHookIcon, icon);
        runtime.registerFunction(kHookRows, rows);
        runtime.registerFunction(kSoundReturn, afterSound);
        runtime.registerFunction(kRowsReturn, continueRows);

        g_area = vendetta_store::fixed(kAreaOffset, kAreaSize, "Choose Match's second step");
        g_original = runtime.lookupFunction(kChooseMatch);
        g_originalBackFrom = runtime.lookupFunction(kBackFrom);
        g_originalSetUp = runtime.lookupFunction(kMatchSetUp);
        g_originalInit = runtime.lookupFunction(kMatchInit);
        if (!g_icon || !g_fontEnd || !g_area || !g_original || !g_originalBackFrom || !g_originalSetUp || !g_originalInit)
        {
            std::cerr << "[cage] Choose Match's routines aren't where the original program has them: no match types"
                      << std::endl;
            return;
        }
        runtime.replaceFunction(kChooseMatch, chooseMatch);
        runtime.replaceFunction(kBackFrom, backFrom);
        runtime.replaceFunction(kMatchSetUp, matchSetUp);
        runtime.replaceFunction(kMatchInit, matchInit);
    }
}

void vendetta_cage::setTestRules(const Rules &rules)
{
    vendetta_cage_menu::g_test = rules;
    vendetta_cage_menu::g_haveTest = true;
}

void installVendettaCageMenu(PS2Runtime &runtime) { vendetta_cage_menu::install(runtime); }
