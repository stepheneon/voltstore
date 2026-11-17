#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace test {

// Each test cleans up only its own temporary directory.
class Workspace {
public:
    explicit Workspace(const std::string& label) {
        const auto pattern = (std::filesystem::temp_directory_path() /
                              ("voltstore-" + label + "-XXXXXX")).string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(),
                                    "create test directory");
        }
        path_ = buffer.data();
    }

    ~Workspace() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> function) {
        registry().push_back({std::move(name), std::move(function)});
    }
};

inline void require(bool condition, const char* expression, const char* file,
                    int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" +
                                 std::to_string(line) + " REQUIRE(" + expression +
                                 ") failed");
    }
}

}  // namespace test

#define TEST_CASE(name)                                    \
    static void name();                                    \
    static test::Registrar registrar_##name(#name, name);  \
    static void name()

#define REQUIRE(expression) \
    test::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
