// xeo3-recomp-kit runtime: the game's short name.
//
// Every log and marker file the runtime reads or writes in the console folder is prefixed
// with it (<game>_crash.log, <game>_autologon.on, ...), so several recompiled games can
// share one console folder without reading each other's switches. scripts\recompile.ps1
// writes a copy of this file with the real name into the game's ppc\ folder; this default
// only applies to a hand build. Keep it short (it is part of fixed-size path buffers).
#pragma once
#ifndef ZZ_TITLE
#define ZZ_TITLE "game"
#endif
