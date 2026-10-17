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

The launcher has Start Game, Netplay, Controls, Graphics & Sound, Add ROM (for ROM
hacks that only change the game's assets) and Exit.

Netplay
  - Everyone must run the same version of this program. The handshake refuses a
    different build, ROM or settings profile.
  - The host opens Netplay, picks the number of players, the port and the address
    friends will use, and sends them the join code. A Tailscale address works
    without opening any ports; otherwise the host must forward the UDP port.
  - The others open Netplay and paste the join code. The game starts on every
    machine once all are in.
  - The join code holds the host's address, so send it only to people you trust.
  - Each player uses their first controller. Mouse and gyro aiming and the free
    camera are off during a match. A match starts from boot with an empty save.

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
