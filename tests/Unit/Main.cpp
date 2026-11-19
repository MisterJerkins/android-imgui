#include "Test.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

std::vector<Test::Case>& Test::Cases() {
    static std::vector<Case> all;
    return all;
}

void Test::Fail(const char* file, int line, const std::string& what) {
    failuresInCase_++;
    std::printf("    %s:%d: %s\n", file, line, what.c_str());
}

int Test::RunAll(const char* filter) {
    int passed = 0, failed = 0;
    for (const Case& one : Cases()) {
        if (!std::strstr(one.name, filter)) {
            continue;
        }
        failuresInCase_ = 0;
        one.run();
        if (failuresInCase_ == 0) {
            passed++;
        } else {
            failed++;
            std::printf("test %s ... FAILED\n", one.name);
        }
    }
    std::printf("test result: %s. %d passed; %d failed\n", failed == 0 ? "ok" : "FAILED", passed, failed);
    return failed;
}

// Runs every test, or those whose names contain the argument.
int main(int argc, char** argv) {
    return Test::RunAll(argc > 1 ? argv[1] : "") == 0 ? 0 : 1;
}
