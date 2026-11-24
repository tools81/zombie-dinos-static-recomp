Zombie Dinos from Planet Zeltoid (Philips CD-i) for Windows
===========================================================

Runs the game from your own disc image. No disc image is included: you need
the .cue and .bin (2352 bytes per sector). Windows 10 (1903) or later, 64-bit.

Starting
--------
Setup:         run ZombieDinos-<version>-setup.exe and choose your disc image.
Portable zip:  unzip it anywhere you can write (not Program Files) and run
               zombiedinos.exe. It asks for the disc image once. Or put the
               image in a "rom" folder beside it and it is found by itself.

Everything stays in this folder
-------------------------------
The game writes only to saves\ beside zombiedinos.exe: game saves, quicksaves,
settings.ini, disc_path.txt (where your disc image is) and recomp.log. It uses
no AppData and no registry. The shaders are built into the program. To move
the game, move the folder. This is always on; --portable in the shortcuts only
says so. (Setup itself adds an uninstall entry and shortcuts, unless you choose
its Portable type.)

Controls
--------
Mouse / arrow keys / gamepad   pointer
Left, right button             button 1, 2 (Space, Z, A / X, S, Ctrl)
F2 / F3 / F4                   quicksave / next slot (1-10) / load
F6 / F7                        run-ahead / sprite interpolation
F9 / F10 / F11                 aspect / filter / full screen
Tab (hold)                     fast forward
Esc                            quit

Updates
-------
At start the game asks GitHub for a newer release. If there is one, it is
downloaded, verified, installed, and the game restarts. Saves and settings are
kept. Without a connection the game just starts. If an update fails or you
close its window, it is tried again a day later.
To turn this off: update=0 in saves\settings.ini, or --no-update-check.
