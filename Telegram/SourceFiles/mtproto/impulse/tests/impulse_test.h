#pragma once

#include <cstdio>
#include <vector>

namespace impulse::test {

struct TestCase {
    const char *name;
    void (*run)();
};

inline std::vector<TestCase> &testRegistry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int &testFailureCount() {
    static int count = 0;
    return count;
}

inline void reportFailure(const char *file, int line, const char *expression) {
    // Called by IMPULSE_CHECK when the expression is false.
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    ++testFailureCount();
}

struct Registrar {
    Registrar(const char *name, void (*run)()) {
        testRegistry().push_back(TestCase{name, run});
    }
};

} // namespace impulse::test

#define IMPULSE_TEST(Name) \
    static void impulseTest##Name(); \
    static const ::impulse::test::Registrar impulseRegistrar##Name(#Name, &impulseTest##Name); \
    static void impulseTest##Name()

#define IMPULSE_CHECK(expr) \
    ((expr) ? static_cast<void>(0) : ::impulse::test::reportFailure(__FILE__, __LINE__, #expr))
