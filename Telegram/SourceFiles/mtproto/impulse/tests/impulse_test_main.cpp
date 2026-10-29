#include "mtproto/impulse/tests/impulse_test.h"

int main() {
    const std::vector<impulse::test::TestCase> &tests = impulse::test::testRegistry();
    const int total = static_cast<int>(tests.size());
    int failedTests = 0;
    for (const impulse::test::TestCase &testCase : tests) {
        const int failuresBefore = impulse::test::testFailureCount();
        std::printf("[ RUN      ] %s\n", testCase.name);
        testCase.run();
        if (impulse::test::testFailureCount() != failuresBefore) {
            ++failedTests;
            std::printf("[  FAILED  ] %s\n", testCase.name);
        } else {
            std::printf("[       OK ] %s\n", testCase.name);
        }
    }
    std::printf("%d of %d tests passed\n", total - failedTests, total);
    return failedTests ? 1 : 0;
}
