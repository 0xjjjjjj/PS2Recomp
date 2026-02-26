namespace
{
    constexpr uint32_t kIntcVblankStart = 2u;
    constexpr uint32_t kIntcVblankEnd = 3u;
    constexpr auto kVblankPeriod = std::chrono::microseconds(16667);
    constexpr int kMaxCatchupTicks = 4;

    struct VSyncFlagRegistration
    {
        uint32_t flagAddr = 0;
        uint32_t tickAddr = 0;
    };

    static std::mutex g_irq_handler_mutex;
    static std::mutex g_irq_worker_mutex;
    static std::mutex g_vsync_flag_mutex;
    static std::atomic<bool> g_irq_worker_stop{false};
    static std::atomic<bool> g_irq_worker_running{false};
    static uint32_t g_enabled_intc_mask = 0xFFFFFFFFu;
    static uint32_t g_enabled_dmac_mask = 0xFFFFFFFFu;
    static uint64_t g_vsync_tick_counter = 0u;
    static VSyncFlagRegistration g_vsync_registration{};

    // Cooperative VBlank: timer thread increments this, main dispatch loop
    // drains it via pollVBlank().  Models PS2 where interrupts fire on the
    // same core at instruction boundaries.
    static std::atomic<int> g_vblank_pending{0};
}

static void writeGuestU32NoThrow(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    if (addr == 0u)
    {
        return;
    }

    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
    {
        return;
    }
    std::memcpy(dst, &value, sizeof(value));
}

static void writeGuestU64NoThrow(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    if (addr == 0u)
    {
        return;
    }

    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
    {
        return;
    }
    std::memcpy(dst, &value, sizeof(value));
}

static void dispatchIntcHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
{
    if (!rdram || !runtime)
    {
        return;
    }

    std::vector<IrqHandlerInfo> handlers;
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (cause < 32u && (g_enabled_intc_mask & (1u << cause)) == 0u)
        {
            return;
        }

        handlers.reserve(g_intcHandlers.size());
        for (const auto &[id, info] : g_intcHandlers)
        {
            (void)id;
            if (!info.enabled)
            {
                continue;
            }
            if (info.cause != cause)
            {
                continue;
            }
            if (info.handler == 0u)
            {
                continue;
            }
            handlers.push_back(info);
        }
    }

    for (const IrqHandlerInfo &info : handlers)
    {
        if (!runtime->hasFunction(info.handler))
        {
            continue;
        }

        try
        {
            R5900Context irqCtx{};
            const uint32_t sp = (info.sp != 0u) ? info.sp : PS2_IRQ_STACK_TOP;
            SET_GPR_U32(&irqCtx, 28, info.gp);
            SET_GPR_U32(&irqCtx, 29, sp);
            SET_GPR_U32(&irqCtx, 31, 0u);
            SET_GPR_U32(&irqCtx, 4, cause);
            SET_GPR_U32(&irqCtx, 5, info.arg);
            SET_GPR_U32(&irqCtx, 6, 0u);
            SET_GPR_U32(&irqCtx, 7, 0u);
            irqCtx.pc = info.handler;

            PS2Runtime::RecompiledFunction func = runtime->lookupFunction(info.handler);
            func(rdram, &irqCtx, runtime);
        }
        catch (const ThreadExitException &)
        {
        }
        catch (const std::exception &e)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 8u)
            {
                std::cerr << "[INTC] handler 0x" << std::hex << info.handler
                          << " threw exception: " << e.what() << std::dec << std::endl;
                ++warnCount;
            }
        }
    }
}

static void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
{
    if (!rdram || !runtime)
    {
        return;
    }

    std::vector<IrqHandlerInfo> handlers;
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (cause < 32u && (g_enabled_dmac_mask & (1u << cause)) == 0u)
        {
            return;
        }

        handlers.reserve(g_dmacHandlers.size());
        for (const auto &[id, info] : g_dmacHandlers)
        {
            (void)id;
            if (!info.enabled)
            {
                continue;
            }
            if (info.cause != cause)
            {
                continue;
            }
            if (info.handler == 0u)
            {
                continue;
            }
            handlers.push_back(info);
        }
    }

    if (handlers.empty())
    {
        std::cerr << "[DMAC] no handlers for cause=" << cause << std::endl;
    }

    for (const IrqHandlerInfo &info : handlers)
    {
        if (!runtime->hasFunction(info.handler))
        {
            std::cerr << "[DMAC] handler 0x" << std::hex << info.handler
                      << " NOT FOUND in runtime (cause=" << std::dec << cause << ")" << std::endl;
            continue;
        }

        try
        {
            std::cerr << "[DMAC] dispatching handler 0x" << std::hex << info.handler
                      << " cause=" << std::dec << cause
                      << " arg=0x" << std::hex << info.arg << std::dec << std::endl;
            R5900Context irqCtx{};
            const uint32_t sp = (info.sp != 0u) ? info.sp : PS2_IRQ_STACK_TOP;
            SET_GPR_U32(&irqCtx, 28, info.gp);
            SET_GPR_U32(&irqCtx, 29, sp);
            SET_GPR_U32(&irqCtx, 31, 0u);
            SET_GPR_U32(&irqCtx, 4, cause);
            SET_GPR_U32(&irqCtx, 5, info.arg);
            SET_GPR_U32(&irqCtx, 6, 0u);
            SET_GPR_U32(&irqCtx, 7, 0u);
            irqCtx.pc = info.handler;

            PS2Runtime::RecompiledFunction func = runtime->lookupFunction(info.handler);
            func(rdram, &irqCtx, runtime);
        }
        catch (const ThreadExitException &)
        {
        }
        catch (const std::exception &e)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 8u)
            {
                std::cerr << "[DMAC] handler 0x" << std::hex << info.handler
                          << " threw exception: " << e.what() << std::dec << std::endl;
                ++warnCount;
            }
        }
    }
}

static void signalVSyncFlag(uint8_t *rdram, uint64_t tickValue)
{
    VSyncFlagRegistration reg{};
    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        reg = g_vsync_registration;
        g_vsync_registration = {};
        g_vsync_tick_counter = tickValue;
    }

    if (reg.flagAddr != 0u)
    {
        writeGuestU32NoThrow(rdram, reg.flagAddr, 1u);
    }
    if (reg.tickAddr != 0u)
    {
        writeGuestU64NoThrow(rdram, reg.tickAddr, tickValue);
    }
}

static void interruptWorkerMain(uint8_t *rdram, PS2Runtime *runtime)
{
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now() + kVblankPeriod;

    while (!g_irq_worker_stop.load(std::memory_order_acquire) &&
           runtime != nullptr &&
           !runtime->isStopRequested())
    {
        std::this_thread::sleep_until(nextTick);

        const auto now = clock::now();
        int ticksToProcess = 0;
        while (now >= nextTick && ticksToProcess < kMaxCatchupTicks)
        {
            ++ticksToProcess;
            nextTick += kVblankPeriod;
        }
        if (ticksToProcess == 0)
        {
            continue;
        }

        // Just set the pending count — INTC dispatch happens on the main
        // thread via pollVBlank(), matching how real PS2 fires interrupts
        // on the same core at instruction boundaries.
        g_vblank_pending.fetch_add(ticksToProcess, std::memory_order_release);

        static uint32_t timerLog = 0;
        ++timerLog;
        if (timerLog <= 20 || (timerLog % 120) == 0)
        {
            std::cerr << "[VBlankTimer] tick#" << timerLog
                      << " added=" << ticksToProcess
                      << " pending=" << g_vblank_pending.load() << std::endl;
        }
    }

    std::cerr << "[VBlankTimer] EXITING! stop="
              << g_irq_worker_stop.load()
              << " stopReq=" << (runtime ? runtime->isStopRequested() : -1)
              << std::endl;
    g_irq_worker_running.store(false, std::memory_order_release);
}

// Called from the main dispatch loop (same thread as guest code).
// Drains pending VBlank ticks and dispatches INTC handlers inline.
// No mutex needed — this runs on the main thread which already owns
// the guest execution context.
static void pollVBlankInline(uint8_t *rdram, PS2Runtime *runtime)
{
    int pending = g_vblank_pending.exchange(0, std::memory_order_acquire);
    if (pending <= 0)
    {
        return;
    }

    // Cap catchup to avoid huge bursts after long mutex holds
    if (pending > kMaxCatchupTicks)
    {
        pending = kMaxCatchupTicks;
    }

    static uint32_t pollLog = 0;
    ++pollLog;

    for (int i = 0; i < pending; ++i)
    {
        uint64_t tickValue = 0u;
        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            tickValue = ++g_vsync_tick_counter;
        }
        signalVSyncFlag(rdram, tickValue);
        if (pollLog <= 30 || (pollLog % 60) == 0)
        {
            std::cerr << "[pollVBlank] tick=" << tickValue
                      << " pending=" << pending << std::endl;
        }
        dispatchIntcHandlersForCause(rdram, runtime, kIntcVblankStart);
        dispatchIntcHandlersForCause(rdram, runtime, kIntcVblankEnd);
    }
}

static void ensureInterruptWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
{
    if (!rdram || !runtime)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_irq_worker_mutex);
    if (g_irq_worker_running.load(std::memory_order_acquire))
    {
        return;
    }

    g_irq_worker_stop.store(false, std::memory_order_release);
    g_irq_worker_running.store(true, std::memory_order_release);
    try
    {
        std::thread(interruptWorkerMain, rdram, runtime).detach();
    }
    catch (...)
    {
        g_irq_worker_running.store(false, std::memory_order_release);
    }
}

void stopInterruptWorker()
{
    g_irq_worker_stop.store(true, std::memory_order_release);
    for (int i = 0; i < 100 && g_irq_worker_running.load(std::memory_order_acquire); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t flagAddr = getRegU32(ctx, 4);
    const uint32_t tickAddr = getRegU32(ctx, 5);

    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        g_vsync_registration.flagAddr = flagAddr;
        g_vsync_registration.tickAddr = tickAddr;
    }

    writeGuestU32NoThrow(rdram, flagAddr, 0u);
    writeGuestU64NoThrow(rdram, tickAddr, 0u);
    ensureInterruptWorkerRunning(rdram, runtime);
    setReturnS32(ctx, KE_OK);
}

void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t cause = getRegU32(ctx, 4);
    if (cause < 32u)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_enabled_intc_mask |= (1u << cause);
    }
    setReturnS32(ctx, KE_OK);
}

void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t cause = getRegU32(ctx, 4);
    if (cause < 32u)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_enabled_intc_mask &= ~(1u << cause);
    }
    setReturnS32(ctx, KE_OK);
}

void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    IrqHandlerInfo info{};
    info.cause = getRegU32(ctx, 4);
    info.handler = getRegU32(ctx, 5);
    info.arg = getRegU32(ctx, 7);
    info.gp = getRegU32(ctx, 28);
    info.sp = 0;  // Use dedicated IRQ stack, not caller's stack
    info.enabled = true;

    int handlerId = 0;
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        handlerId = g_nextIntcHandlerId++;
        g_intcHandlers[handlerId] = info;
    }

    std::cerr << "[AddIntcHandler] id=" << handlerId
              << " cause=" << info.cause
              << " handler=0x" << std::hex << info.handler
              << " arg=0x" << info.arg << std::dec << std::endl;

    ensureInterruptWorkerRunning(rdram, runtime);
    setReturnS32(ctx, handlerId);
}

void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int handlerId = static_cast<int>(getRegU32(ctx, 5));
    if (handlerId > 0)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_intcHandlers.erase(handlerId);
    }
    setReturnS32(ctx, KE_OK);
}

void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    IrqHandlerInfo info{};
    info.cause = getRegU32(ctx, 4);
    info.handler = getRegU32(ctx, 5);
    info.arg = getRegU32(ctx, 7);
    info.gp = getRegU32(ctx, 28);
    info.sp = 0;  // Use dedicated IRQ stack, not caller's stack
    info.enabled = true;

    int handlerId = 0;
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        handlerId = g_nextDmacHandlerId++;
        g_dmacHandlers[handlerId] = info;
    }
    std::cerr << "[AddDmacHandler] id=" << handlerId
              << " cause=" << info.cause
              << " handler=0x" << std::hex << info.handler
              << " arg=0x" << info.arg << std::dec << std::endl;
    setReturnS32(ctx, handlerId);
}

void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int handlerId = static_cast<int>(getRegU32(ctx, 5));
    if (handlerId > 0)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_dmacHandlers.erase(handlerId);
    }
    setReturnS32(ctx, KE_OK);
}

void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int handlerId = static_cast<int>(getRegU32(ctx, 5));
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
        {
            it->second.enabled = true;
        }
    }
    setReturnS32(ctx, KE_OK);
}

void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int handlerId = static_cast<int>(getRegU32(ctx, 5));
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
        {
            it->second.enabled = false;
        }
    }
    setReturnS32(ctx, KE_OK);
}

void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int handlerId = static_cast<int>(getRegU32(ctx, 5));
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
        {
            it->second.enabled = true;
        }
    }
    setReturnS32(ctx, KE_OK);
}

void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int handlerId = static_cast<int>(getRegU32(ctx, 5));
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
        {
            it->second.enabled = false;
        }
    }
    setReturnS32(ctx, KE_OK);
}

void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t cause = getRegU32(ctx, 4);
    if (cause < 32u)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_enabled_dmac_mask |= (1u << cause);
    }
    setReturnS32(ctx, KE_OK);
}

void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t cause = getRegU32(ctx, 4);
    if (cause < 32u)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_enabled_dmac_mask &= ~(1u << cause);
    }
    setReturnS32(ctx, KE_OK);
}
