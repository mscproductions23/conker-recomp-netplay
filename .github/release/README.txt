Conker's Bad Fur Day: Recompiled, Netplay Edition

A fork of https://github.com/sciaschi/CBFD-Recompiled that adds online play with
friends. The netplay itself is upstream's work, by easymod0.

This package contains no game data. You need your own copy of the US ROM of
Conker's Bad Fur Day (N64), in .z64 format.

Playing
  1. Start ConkerRecomp (ConkerRecomp.exe on Windows, ConkerRecomp.app on macOS).
  2. The first time, the first launcher option is Load ROM: pick your US .z64 ROM.
     It's checked, and remembered for later runs. It then reads Start Game.
  3. Start Game for the single-player game and the game's own multiplayer, or
     Netplay to play online.

The launcher has Start Game, Netplay, Controls, Controller Test, Graphics & Sound,
Add ROM (for ROM hacks that only change the game's assets) and Exit.

The menus name the buttons for your controller (Xbox, PlayStation or Nintendo).
Controller Test shows every button, stick and trigger live: hold Start or Esc for
a second and a half to leave it.

Netplay
  - Everyone must run the same version of this program. The handshake refuses a
    different build, ROM or settings profile (the lobby says which).
  - The host opens Netplay, picks the number of players, the port and the address
    friends will use (Tailscale and home-network addresses are filled in for you),
    and presses Host. Everyone then meets in a lobby.
  - Friends join with the host's join code (paste it, or it is filled in from the
    clipboard). If someone runs a room server, its address goes under "Room server"
    on the Netplay page, the host also gets a short code such as K7Q-F2M, and friends
    can type that instead. A Tailscale address works without opening any ports; the
    program also asks the router to forward the port (UPnP) and says when it can't.
  - In the lobby each player sees the others' names, controller types, ping and a
    live view of their controller, so a bad pad or connection shows before the game
    starts. Players press "I'm ready"; the host picks the input delay (Auto is
    recommended from everyone's ping) and, if they like, a game mode (Deathmatch,
    War, Tank, Race, Beach, Raptor or Heist), which takes everyone straight into
    that match. "Choose in game" leaves the menus to the players. Then Start game.
  - "Copy diagnostics" puts a report on the clipboard to send with a bug report.
  - The join code holds the host's address, so send it only to people you trust.
  - Each player uses their first controller. Mouse and gyro aiming and the free
    camera are off during a match. A match starts from boot with an empty save.

Testing this build
  Try, between two machines: (1) host and join by the long join code and get to the
  lobby; check you see each other's name, controller type, ping and live controller,
  then Ready and Start; (2) host with a game mode picked (War or Race are quick);
  (3) with a room server (tools/room_server in the source repository), join by the
  short code; (4) join with a different build on purpose: the lobby says why it was
  refused. If something fails, press "Copy diagnostics" on each machine and send the
  text with what you were doing.

Requirements
  Windows: Windows 10 or 11, 64-bit, with a Direct3D 12 or Vulkan graphics card.
  Linux: x86-64 with Vulkan, and SDL2, GTK 3 and FreeType installed (on Ubuntu or
         Debian: sudo apt install libsdl2-2.0-0 libgtk-3-0 libfreetype6).
         Start it with the ConkerRecomp script in this folder.
  macOS: Apple Silicon, macOS 15 or later. The app isn't signed with an Apple
         developer ID, so macOS blocks it the first time: open it once, then in
         System Settings > Privacy & Security choose Open Anyway. Or, in Terminal,
         run xattr -dr com.apple.quarantine ConkerRecomp.app before opening it.

Conker's Bad Fur Day is (c) Rare Ltd. This project is not affiliated with or
endorsed by Rare, Microsoft or Nintendo. The licenses of the parts of this program
are in LICENSE, LICENSES.txt and LICENSE-GPL-3.0.txt.
