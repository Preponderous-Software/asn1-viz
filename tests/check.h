// A minimal test harness: CHECK records a failure and keeps going, and main()
// returns non-zero when anything failed, so ctest reports every broken
// expectation at once.
#pragma once

#include <cstdio>

namespace asn1viz_test {
inline int failures = 0;
inline int checks = 0;
}  // namespace asn1viz_test

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++asn1viz_test::checks;                                                           \
        if (!(cond)) {                                                                    \
            ++asn1viz_test::failures;                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

#define TEST_MAIN_END()                                                                   \
    std::printf("%d checks, %d failed\n", asn1viz_test::checks, asn1viz_test::failures);  \
    return asn1viz_test::failures == 0 ? 0 : 1
