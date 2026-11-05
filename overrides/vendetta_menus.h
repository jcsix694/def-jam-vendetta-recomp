// Def Jam Vendetta (USA, SLUS-20639) — menu themes: a mod changing how the game's menu screens look.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A menu theme is a mod whose mod.json has a "menus" block:
//
//   "menus": { "format": 1, "shared": "menus/shared.json", "screens": { "main-menu": "menus/main-menu/screen.json" } }
//
// shared.json holds the pieces that are on several screens (changed on every one of them), plus "highlight" and
// "fonts"; a screen's file holds that screen's own pieces. Both: { "pieces": { "<piece>": { ... } } }. A piece is
// named by the catalogue (config/menu_pieces.csv, built into the game as vendetta_menu_pieces.inc); in a screen's file
// its own pieces may drop the screen's name. A piece's keys: "picture" (a PNG, any size, relative to the file),
// "fit" ("stretch" the default / "fit" inside, centred / "fill" cut to the box), "colour" ("game" the default, "white",
// or [r, g, b]), "shape" (a backdrop's "4:3" / "16:9"); words and fonts come later. A theme with a higher format than
// this game knows is skipped whole; two themes changing one piece: the later one in the project wins it.
//
// The game still runs every screen; only how its pictures look changes. Every menu picture is one quad through
// FUN_00183c88 (its texture set by the pickers FUN_00157570 / FUN_00184d78): the picker's source and the quad's UV
// rectangle name the piece; a changed one is drawn from the mod's picture, bound at its GS address with register 0x71
// around the game's own draw. With no menu theme nothing is hooked. VENDETTA_MENUS=0: themes off. VENDETTA_MENULOG=1:
// every changed piece the first time it is drawn, and every quad that matches no catalogue piece (a gap in it).
#pragma once

class PS2Runtime;

void installVendettaMenus(PS2Runtime &runtime);
