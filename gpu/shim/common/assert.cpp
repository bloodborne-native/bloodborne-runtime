#include <csetjmp>
// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#include <unistd.h>

// bb-probe (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
extern "C" __attribute__((weak)) volatile int runtime_restarting; // absent in the tests

thread_local sigjmp_buf* bb_assert_recover = nullptr; // local experiment: the list step skips a program

void assert_fail_impl() {
    if (bb_assert_recover) {
        siglongjmp(*bb_assert_recover, 2);
    }
    if (&runtime_restarting && runtime_restarting) {
        for (;;) {
            pause();
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
