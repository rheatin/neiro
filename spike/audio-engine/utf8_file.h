// UTF-8 paths for the container readers.
//
// The engine and its callers speak UTF-8; Windows' narrow fopen uses the ANSI
// code page, so a CJK path opens the wrong file or fails. The track decoder goes
// through FFmpeg and is unaffected; the DSF/DFF readers are not.

#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neiro {

inline std::FILE* openUtf8(const char* path, const char* mode) {
#ifdef _WIN32
    const int pathLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    const int modeLen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    if (pathLen <= 0 || modeLen <= 0) return nullptr;
    std::wstring widePath(static_cast<size_t>(pathLen), L'\0');
    std::wstring wideMode(static_cast<size_t>(modeLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), pathLen);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wideMode.data(), modeLen);
    return _wfopen(widePath.c_str(), wideMode.c_str());
#else
    return std::fopen(path, mode);
#endif
}

}  // namespace neiro
