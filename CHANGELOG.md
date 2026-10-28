# Release notes

Each version's notes: what's new, what's better, what's fixed, and anything good to know before updating.
The newest version is at the top.

---

## 0.4.4

### Fixed
- **Small clicks in the sound.** The sound had faint clicks a few times a second, in the menus, the movies and the
  matches, on every PC. They're gone. Thanks to the player who recorded them and pointed them out.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.4.3

### Better
- **Borderless replaces Fullscreen.** The game now plays in a window or borderless. Borderless fills your screen just
  like Fullscreen did, and switching to other programs is instant. If you played in Fullscreen, the game switches you
  to borderless by itself the first time you start it.

### Good to know
- Borderless keeps your screen at the refresh rate set in Windows. On a 144 or 165 Hz monitor without G-Sync or
  FreeSync, setting Windows to 120 Hz (if your monitor has it) makes the game's movement smoothest.
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.4.2

### Fixed
- **Crackling sound on some PCs.** On some PCs the sound crackled all the time, in the menus and in matches, even
  though other games sounded fine. The game now hands its sound to Windows in bigger pieces, which stops it. You
  won't hear any delay.

### Better
- **New installs start borderless.** A first install now opens in a borderless window the size of your screen, with
  widescreen and VSync on. If you already play, your settings stay as they are. You can switch between borderless,
  fullscreen and a window on the settings program's Display page.

### Good to know
- On a 144 or 165 Hz monitor, **Fullscreen** can look a little smoother than borderless, because it can switch your
  screen to a rate that suits the game.
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.4.1

### Fixed
- **Setup works on PCs whose processor doesn't support AVX2.** On these PCs the setup stopped at "Recompiling the
  game" with exit code 3221225501. The setup now checks your processor and builds the game to suit it. PCs with AVX2
  get the same game as before.
- **No slowdown on those PCs.** The game built for a processor without AVX2 now draws as fast as the normal one, 4
  player matches included.

### Good to know
- If the setup stopped for you before, download this version, unzip it and run the setup again.
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.4.0

### New
- **Updating by hand from the zip is spotted.** If you unzip a new version over your install instead of using Update,
  the settings program now notices that the game still needs building and shows "The game needs rebuilding" on its
  Play page. Press **Rebuild now** and it is done for you. From this version on, the game also asks Rebuild or Play
  when it starts in that case. Your saves and settings stay as they are.
- **The logo in the window's title bar.** In a window, the game now shows the Def Jam Vendetta Recompiled logo in the
  top left corner, the same one as on the taskbar.

### Better
- **WWF No Mercy's pace is now 0.86** of the game's own speed (it was 0.862), a rounder number for the same feel.
- **New settings slot into older settings files neatly.** A setting a new version adds to your settings file now goes
  in the right place, with its own explanation.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.3.0

### New
- **Match speed.** A new setting on the settings program's Gameplay page. Choose Def Jam Vendetta's own pace (the
  default) or WWF No Mercy's slower pace. Turn on Custom speed to pick any speed from 0.750 to 1.250 with the slider,
  or type it in. It only changes the fight itself, from the moment it starts until the match is decided. Intros,
  menus and everything after stay at normal speed. The match clock follows the chosen speed.

### Fixed
- **Snipping Tool works in fullscreen.** Opening Snipping Tool over the game no longer drops the game to the taskbar,
  so screenshots and screen recordings show the game instead of the window behind it.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- **Updating by hand from the zip?** The zip doesn't contain the game itself: the game is built on your PC from your
  disc, and only the update in the settings program rebuilds it. Unzipping a new version over your install keeps the
  old game. To fix it, close the game and the settings program, then in the `files` folder delete
  `Def Jam Vendetta Recompiled.exe` **and** the `build` folder. Open the settings program and press build (about 10 to
  15 minutes, as everything is built afresh). Your saves and settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.2.2

### Fixed
- **No more lines or a square on the mat after a Blazin'.** At the higher sharpness settings, thin lines or a large
  darker square could appear on the floor and follow the wrestler after a Blazin'. The wrestlers' shadows are drawn
  correctly again, as on the PS2.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.2.1

### Fixed
- **The first setup works in every time zone.** Outside the UK, for example in the USA, the setup could stop at step 3
  with "build.ninja still dirty". The download's files now carry a date that is in the past everywhere, and the setup
  also corrects any file dated in the future before it builds (a PC clock that is set wrong did the same).
- **Fullscreen stays in front when Copilot jumps in.** When the screen switches to the game's refresh rate, Microsoft
  Copilot's Quick View can take the front by itself. The game now takes the front back on its own in the first few
  seconds, unless you are pressing something, so you no longer need to click it on the taskbar.

### Good to know
- If the setup of 0.2.0 stopped for you at step 3, download this version and run it again.
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.2.0

### New
- **Quit on the main menu.** The main menu has a new item at the end, with the game's own EXIT picture. Press Cross
  on it and the game asks "Quit the game?". Cross closes the game, Triangle goes back.
- **Refresh rate "Automatic", the new default for new installs.** It keeps your screen's own rate when that already
  suits the game (60, 120 or 240 Hz, or a TV's 59.94), so nothing switches. Otherwise it picks the smoothest rate your
  screen has. It's worked out every time the game starts, so a new monitor or TV is handled too. If you installed an
  earlier version, you can choose it under Display.

### Better
- **The first setup copes when the build stops.** If the build stops partway, often because the PC ran short of
  memory or an antivirus held a file, it now tries again by itself, more slowly, and carries on from where it stopped.
- **Clearer errors.** If the setup or an update can't finish, it says which step stopped, shows the build's own error
  lines and gives advice for the common causes. You can copy the text, and the setup log now keeps every try, so
  problems are easy to report.
- The update notice on the Play page lists a new version's main points one per line.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.3

### Fixed
- **Fullscreen no longer drops you back to the desktop.** Some programs, like Microsoft Copilot, jump to the front when
  the screen changes its refresh rate, which sent the game to the background and made Alt-Tab bounce straight back
  out. The game now stays fullscreen when another program does that, and only minimises when you switch away
  yourself (Alt-Tab, the Windows key or a click elsewhere).
- Pressing **PLAY** in the settings program brings the game to the front reliably.

### Better
- **New installs use your screen's own refresh rate** in fullscreen, so the screen doesn't switch at all. You can still
  pick "Smoothest" or a fixed rate under Display. If you installed an earlier version, your setting stays as it is.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.2

### Better
- **Clearer progress while the game is rebuilt,** in updates and in the first setup. The bar keeps moving through the
  longest part, the line under it says what is happening in plain words (for example "Checking which files changed:
  13,000 of 25,467 files"), and a time left is only shown when it can be worked out. No more "1 min left" while it
  is still busy for several minutes.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.1

### Fixed
- **Widescreen:** crowd members at the sides of the picture no longer vanish and pop back in. The whole crowd is now
  drawn across the full wide picture.

### Good to know
- To update, press **Update** when the game asks as it starts, or on the settings program's Play page. Your saves and
  settings stay as they are.
- After the update Windows may show "Windows protected your PC" again (the program isn't signed yet): click
  **More info**, then **Run anyway**.
- Mod support is actively being worked on and will come in a future update.

## 0.1.0: the first release

Def Jam Vendetta (PlayStation 2, USA, SLUS-20639) as a native Windows program, built on your PC from your own disc.
No game files come with this download.

### New
- **Built from your own disc.** Def Jam Vendetta Recompiled Settings reads your copy (an `.iso` or `.chd` file), installs Microsoft's
  build tools if your PC doesn't have them, and builds the game in 20 to 30 minutes. It only does this once.
- **Up to 4 players**
- **Widescreen** matches (menus and movies stay 4:3), and fullscreen, borderless or windowed play.
- **A sharp picture:** drawn on your graphics card at up to 16 times the PS2's detail, with the HUD, text and menus
  as sharp as the 3D (Sharp 2D).
- **High Poly in every match:** full-detail wrestlers and arenas in 3- and 4-fighter matches too.
- **Camera styles:** Def Jam Vendetta's own camera, WWF No Mercy style or Wrestle Kingdom 2 style (the ring seen from a
  corner, with the controls turned to match). Choose it under Gameplay.
- **Unlock everything:** every wrestler, attire, arena and gallery picture for every player, with or without a save.
  Your save is never changed, and what you unlock while playing is still saved. Off by default, under Gameplay.
- **Vulkan:** the PS2's graphics are redrawn on your graphics card with Vulkan, and the picture is shown with Vulkan too.
- **Def Jam Vendetta Recompiled Settings:** display, graphics, sound, controls and gameplay in one place, with PLAY.

### Good to know
- You need: Windows 10 or 11 (64-bit), a graphics card with Vulkan 1.3, about 8 GB of free space, and your own
  copy of Def Jam Vendetta for PlayStation 2 (USA).
- Unzip somewhere short, like your Desktop or `C:\Games`. The build tools can't handle very long folder paths.
- Windows may say "Windows protected your PC" the first time: click **More info**, then **Run anyway**.
- 16× (the default) needs a fairly strong graphics card. If the game runs slowly, set Scale to 8× under Graphics.
- The keyboard plays as nobody by default: choose a player under Controls.
- Mod support is actively being worked on and will come in a future update.
- When a new version comes out, the game asks when it starts: **Update** opens the settings program on what's new,
  **Play** starts the game and doesn't ask about that version again. Turn it off under About.
- After each update Windows may show "Windows protected your PC" again (the program isn't signed yet): **More info** →
  **Run anyway**.
