#pragma once

#include <cstdio>
#include <cstdlib>

// Always-on test check. Unlike assert(), the condition is evaluated and
// enforced in Release (NDEBUG) builds as well.
#define SX_REQUIRE(cond)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                  \
            std::fflush(stderr);                                                  \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)

#define SX_CHECK(cond) SX_REQUIRE(cond)
