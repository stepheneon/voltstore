#include "test_harness.h"

#include <exception>
#include <iostream>

int main() {
    int failures = 0;

    for (const auto& test_case : test::registry()) {
        try {
            test_case.fn();
            std::cout << "[PASS] " << test_case.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name << ": unexpected exception\n";
        }
    }

    std::cout << "tests=" << test::registry().size()
              << " failed=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
