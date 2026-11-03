// argv as UTF-8 on every platform.
//
// MinGW's narrow `main(argc, argv)` converts the Windows command line to the
// system ANSI code page, not UTF-8. A CJK path then reaches the engine already
// mangled, and the failure looks like "cannot open this file" rather than "the
// path was transcoded before it got here". `open_check.cpp` documents the hour
// that cost.
//
// On Windows the wide command line is the authority, so read it and convert once.
// POSIX already hands main() UTF-8 bytes.

#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

inline std::vector<std::string> utf8Args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> out;
    if (!wide) return out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
        if (n > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), n, nullptr, nullptr);
        }
        out.push_back(std::move(s));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}
