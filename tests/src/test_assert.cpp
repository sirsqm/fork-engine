// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/Assert.h"

#include <csetjmp>
#include <string>

using namespace fe;

namespace {

struct AssertRecord {
    bool called = false;
    std::string expression;
    std::string message;
};

AssertRecord g_record;
std::jmp_buf g_jump;

// A failed assertion is reported through a [noreturn] noexcept function, so the
// handler cannot throw (that would call std::terminate) and cannot simply
// return (assert_fail aborts). longjmp is the standard way for a test harness
// to unwind out of a fatal error path and keep running.
void jumping_handler(const char* expression, const char* file, int line, const char* message) {
    (void)file;
    (void)line;
    g_record.called = true;
    g_record.expression = expression != nullptr ? expression : "";
    g_record.message = message != nullptr ? message : "";
    std::longjmp(g_jump, 1);
}

class ScopedAssertHandler {
public:
    ScopedAssertHandler() {
        g_record = {};
        set_assert_handler(&jumping_handler);
    }
    ~ScopedAssertHandler() { set_assert_handler(nullptr); }
};

// Evaluates the failing check and returns true if the handler fired. The
// handler longjmps back to this point. The macro is used as a statement so the
// assertion macro's own `do { ... } while (false)` does not need to appear in a
// comma expression.
bool check_fires() {
    if (setjmp(g_jump) == 0) {
        FE_CHECK(1 == 2);
        return false;
    }
    return true;
}

bool check_msg_fires() {
    if (setjmp(g_jump) == 0) {
        FE_CHECK_MSG(false, "custom context");
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("FE_CHECK fires the handler with the expression text") {
    ScopedAssertHandler scoped;

    CHECK(check_fires());
    CHECK(g_record.called);
    CHECK(g_record.expression == "1 == 2");
}

TEST_CASE("FE_CHECK does not fire when the expression holds") {
    ScopedAssertHandler scoped;
    FE_CHECK(2 == 2);
    CHECK_FALSE(g_record.called);
}

TEST_CASE("FE_CHECK_MSG forwards the message") {
    ScopedAssertHandler scoped;

    CHECK(check_msg_fires());
    CHECK(g_record.called);
    CHECK(g_record.message == "custom context");
}

TEST_CASE("the assert handler is installed and can be restored") {
    {
        ScopedAssertHandler scoped;
        CHECK(assert_handler() != nullptr);
    }
    // After the scope, the default handler is back in place. It is not invoked,
    // because it aborts the process.
    CHECK(assert_handler() != nullptr);
}
