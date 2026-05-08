#pragma once

#ifdef BEACON_STATIC
    #define BEACON_API
#elif defined(_WIN32) || defined(_WIN64)
    #ifdef BEACON_EXPORTS
        #define BEACON_API __declspec(dllexport)
    #else
        #define BEACON_API __declspec(dllimport)
    #endif
#else
    #ifdef BEACON_EXPORTS
        #define BEACON_API __attribute__((visibility("default")))
    #else
        #define BEACON_API
    #endif
#endif
