#pragma once

#if defined(_WIN32)
#if defined(SCOPEWRITER_EXPORTS)
#define SCOPEWRITER_API __declspec(dllexport)
#else
#define SCOPEWRITER_API __declspec(dllimport)
#endif
#else
#define SCOPEWRITER_API
#endif
