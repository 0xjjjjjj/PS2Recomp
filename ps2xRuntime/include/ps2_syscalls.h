#ifndef PS2_SYSCALLS_H
#define PS2_SYSCALLS_H

#include "ps2_runtime.h"
#include "ps2_call_list.h"
#include <mutex>
#include <atomic>
#include <thread>

// Number of active host threads spawned for PS2 thread emulation
extern std::atomic<int> g_activeThreads;

static std::mutex g_sys_fd_mutex;

namespace ps2_syscalls
{
#define PS2_DECLARE_SYSCALL(name) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    PS2_SYSCALL_LIST(PS2_DECLARE_SYSCALL)
#undef PS2_DECLARE_SYSCALL

    bool dispatchNumericSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void TODO(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t encodedSyscallId);
    void notifyRuntimeStop();
    void dispatchDmacForChannel(uint8_t *rdram, PS2Runtime *runtime, uint32_t channelBase);

    // Drain pending VBlank ticks and dispatch INTC handlers inline.
    // Must be called from the main dispatch loop (same thread as guest code).
    void pollVBlank(uint8_t *rdram, PS2Runtime *runtime);

    // Register the calling thread as the main dispatch thread.
    // Only the main thread polls VBlank in cooperative WaitSema.
    void setMainThread();
    bool isMainThread();

    // Guest execution mutex — serializes guest code on shared rdram.
    // PS2 EE is single-core; all guest threads must hold this while running.
    // Release before blocking waits, reacquire after waking.
    std::mutex& getGuestExecMutex();
}

#endif // PS2_SYSCALLS_H
