/**
 * @file        rex/runtime_api.h
 * @brief       Linkage for data that rexruntime defines and other modules read
 */

#pragma once

// REX_RUNTIME_DATA marks a variable that rexruntime defines and code in other
// modules (the generated game code, GPU plugins, tools) reads directly, such
// as a flag an inline hot path tests.
//
// On Windows rexruntime is a DLL built with WINDOWS_EXPORT_ALL_SYMBOLS. That
// exports its data too, but a module importing data must declare it
// __declspec(dllimport): the import library only provides the __imp_ pointer,
// so a plain extern reference does not link. Targets linking rexruntime get
// REX_RUNTIME_IMPORTS (an INTERFACE definition); rexruntime's own sources and
// the object libraries compiled into it do not, so they see a plain
// definition. Elsewhere this is empty: an ELF or Mach-O shared library's
// non-inline global already has one definition every module binds to.
#if defined(_WIN32) && defined(REX_RUNTIME_IMPORTS)
#define REX_RUNTIME_DATA __declspec(dllimport)
#else
#define REX_RUNTIME_DATA
#endif
