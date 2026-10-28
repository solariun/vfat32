// vfat32 tests — runner. Usage: vfat32_tests [substring]  (runs the tests whose name contains it)
// VFAT32_LONG=1 makes the random stress test run long.

#include <time.h>

#include "test_harness.hpp"

namespace test {

static test_case*& head() {
    static test_case* h = nullptr;
    return h;
}
static test_case*& tail() {
    static test_case* t = nullptr;
    return t;
}
static int failures_in_test = 0;

test_case::test_case(const char* test_name, const char* file_name, test_function fn)
    : name(test_name), file(file_name), function(fn), next(nullptr) {
    if (tail())
        tail()->next = this;
    else
        head() = this;
    tail() = this;
}

test_case* first_test() { return head(); }

void report_failure(const char* file, int line, const char* message) {
    ++failures_in_test;
    printf("    %s:%d: %s\n", file, line, message);
}

int current_failures() { return failures_in_test; }

bool long_mode() {
    const char* v = getenv("VFAT32_LONG");
    return v && v[0] && strcmp(v, "0") != 0;
}

static void print_value(const value& v) {
    if (v.negative)
        printf("-%llu", v.magnitude);
    else
        printf("%llu", v.magnitude);
    if (v.text) printf(" (%s)", v.text);
}

bool check_eq(const value& a, const value& b, const char* ea, const char* eb, const char* file, int line) {
    if (a.negative == b.negative && a.magnitude == b.magnitude) return true;
    ++failures_in_test;
    printf("    %s:%d: CHECK_EQ(%s, %s): ", file, line, ea, eb);
    print_value(a);
    printf(" != ");
    print_value(b);
    printf("\n");
    return false;
}

bool check_str(const char* a, const char* b, const char* ea, const char* eb, const char* file, int line) {
    if (a && b && strcmp(a, b) == 0) return true;
    ++failures_in_test;
    printf("    %s:%d: CHECK_STR(%s, %s): \"%s\" != \"%s\"\n", file, line, ea, eb, a ? a : "(null)", b ? b : "(null)");
    return false;
}

void print_report(const vfat32::check_report& r) {
    printf("    check_report: files %u dirs %u used %u free %u bad %u extents %u fragmented %u\n", static_cast<unsigned>(r.files),
           static_cast<unsigned>(r.directories), static_cast<unsigned>(r.used_clusters), static_cast<unsigned>(r.free_clusters), static_cast<unsigned>(r.bad_clusters), static_cast<unsigned>(r.free_extents), static_cast<unsigned>(r.fragmented));
    printf("    errors: reserved_bits %u bad_chains %u cross_links %u size_errors %u bad_entries %u lost %u "
           "fat_mismatches %u fsinfo %u repaired %u\n",
           static_cast<unsigned>(r.reserved_bits), static_cast<unsigned>(r.bad_chains), static_cast<unsigned>(r.cross_links), static_cast<unsigned>(r.size_errors), static_cast<unsigned>(r.bad_entries), static_cast<unsigned>(r.lost_clusters),
           static_cast<unsigned>(r.fat_mismatches), static_cast<unsigned>(r.fsinfo_errors), static_cast<unsigned>(r.repaired));
}

}  // namespace test

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0;
    int failed = 0;
    for (test::test_case* t = test::first_test(); t; t = t->next) {
        if (filter && !strstr(t->name, filter)) continue;
        test::failures_in_test = 0;
        clock_t start = clock();
        printf("[ RUN  ] %s\n", t->name);
        fflush(stdout);
        t->function();
        double ms = 1000.0 * static_cast<double>(clock() - start) / CLOCKS_PER_SEC;
        if (test::failures_in_test) {
            ++failed;
            printf("[ FAIL ] %s (%d failure%s, %.0f ms)\n", t->name, test::failures_in_test,
                   test::failures_in_test == 1 ? "" : "s", ms);
        } else {
            ++passed;
            printf("[  OK  ] %s (%.0f ms)\n", t->name, ms);
        }
        fflush(stdout);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
