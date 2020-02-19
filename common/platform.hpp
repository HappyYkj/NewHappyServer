#pragma once

#define PLATFORM_UNKNOWN        0
#define PLATFORM_WINDOWS        1
#define PLATFORM_LINUX          2
#define PLATFORM_MAC            3

#define TARGET_PLATFORM         PLATFORM_UNKNOWN

// mac
#if defined(__APPLE__) && (defined(__GNUC__) || defined(__xlC__) || defined(__xlc__))
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_MAC
#endif

// win32
#if !defined(SAG_COM) && (defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__))
#if defined(WINCE) || defined(_WIN32_WCE)
//win ce
#else
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_WINDOWS
#endif
#endif

#if !defined(SAG_COM) && (defined(WIN64) || defined(_WIN64) || defined(__WIN64__))
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_WINDOWS
#endif

// linux
#if defined(__linux__) || defined(__linux)
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_LINUX
#endif

// check user set platform
#if ! TARGET_PLATFORM
#error  "Cannot recognize the target platform; are you targeting an unsupported platform?"
#endif

#include <thread>

//Return current thread id as size_t (from thread local storage)
inline size_t thread_id()
{
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}
