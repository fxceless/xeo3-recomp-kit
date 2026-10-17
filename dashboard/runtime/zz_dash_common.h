// xeo3-recomp-kit dashboard runtime: shared helpers (implemented in zz_dash_init.cpp).
//
// Every file the runtime reads or writes lives in the <console folder> = the directory of
// the running Emu.exe. The directory of the exe, not the working directory: the title
// switcher starts the next Emu through zz_switch.exe, and the exe's own folder is the one
// thing that is the same for every process in that chain.
#pragma once
#include <stddef.h>

// One line to <console folder>\dash_runtime.log (opened shared, so it can be read while the
// dashboard runs) and to the debugger output.
extern "C" void zz_dash_log(const char* line);
extern "C" void zz_dash_logf(const char* fmt, ...);
// <console folder>\<name>
extern "C" void zz_dash_path(char* out, size_t n, const char* name);
// 1 when <console folder>\<name> exists. Callers cache the answer: markers are read once.
extern "C" int zz_dash_marker(const char* name);
