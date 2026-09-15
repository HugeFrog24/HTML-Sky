// ----------------------------------------------------------------------------
// The smallest test harness that still fails loudly.
//
// No framework: this tree builds with a bare mingw g++ invocation from the
// Makefile, and adding a dependency to run three dozen assertions would cost
// more than it returns. A failure prints the file, the line, and both values,
// because "CHECK failed" without the values sends you back to the debugger for
// something the harness already knew.
// ----------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace Check {

struct Case {
  const char *name;
  void (*fn)();
};

inline std::vector<Case> &registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int &failures() {
  static int n = 0;
  return n;
}

inline int &checks() {
  static int n = 0;
  return n;
}

struct Register {
  Register(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char *file, int line, const std::string &what) {
  ++failures();
  std::printf("  FAIL %s:%d\n    %s\n", file, line, what.c_str());
}

inline std::string show(const std::string &v) { return "\"" + v + "\""; }
inline std::string show(const char *v) { return v ? show(std::string(v)) : "<null>"; }
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(int v) { return std::to_string(v); }
inline std::string show(unsigned v) { return std::to_string(v); }
inline std::string show(unsigned long v) { return std::to_string(v); }
inline std::string show(std::size_t v) { return std::to_string(v); }

inline int run(const char *suite) {
  std::printf("%s\n", suite);
  for (const Case &c : registry()) {
    const int before = failures();
    std::printf("- %s\n", c.name);
    c.fn();
    if (failures() != before) std::printf("  ^ in: %s\n", c.name);
  }
  std::printf("\n%d checks, %d failed\n", checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}

#define TEST_CASE(name)                                                        \
  static void name();                                                          \
  static ::Check::Register reg_##name(#name, name);                            \
  static void name()

#define CHECK(expr)                                                            \
  do {                                                                         \
    ++::Check::checks();                                                       \
    if (!(expr)) ::Check::fail(__FILE__, __LINE__, "expected: " #expr);        \
  } while (0)

#define CHECK_EQ(actual, expected)                                             \
  do {                                                                         \
    ++::Check::checks();                                                       \
    const auto a_ = (actual);                                                  \
    const auto e_ = (expected);                                                \
    if (!(a_ == e_))                                                           \
      ::Check::fail(__FILE__, __LINE__,                                        \
                    std::string(#actual) + "\n      got: " +                   \
                      ::Check::show(a_) + "\n      want: " +                   \
                      ::Check::show(e_));                                      \
  } while (0)

#define REQUIRE(expr)                                                          \
  do {                                                                         \
    ++::Check::checks();                                                       \
    if (!(expr)) {                                                             \
      ::Check::fail(__FILE__, __LINE__, "required: " #expr);                   \
      return;                                                                  \
    }                                                                          \
  } while (0)
