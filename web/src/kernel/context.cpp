#include "context.h"

#include <cstdlib>
#include <cstring>

#include "raw_alloc.h"

namespace cardenza_web::kernel {

namespace {
constexpr uint8_t kStackFill = 0xA5;
}

#if defined(__EMSCRIPTEN__)

namespace {
// Asyncify's buffer for the scheduler's own context: it is unwound whenever a task runs.
alignas(16) uint8_t g_mainAsync[64 * 1024];
}

void ContextInitMain(Context* context)
{
    emscripten_fiber_init_from_current_context(&context->fiber, g_mainAsync, sizeof(g_mainAsync));
}

bool ContextCreate(Context* context, void (*entry)(void*), void* arg, std::size_t stackBytes, std::size_t asyncBytes)
{
    context->stack = RawAlloc(stackBytes, 16);
    context->asyncStack = RawAlloc(asyncBytes, 16);
    if (context->stack == nullptr || context->asyncStack == nullptr)
    {
        ContextDestroy(context);
        return false;
    }
    std::memset(context->stack, kStackFill, stackBytes);
    context->stackBytes = stackBytes;
    context->asyncBytes = asyncBytes;
    emscripten_fiber_init(&context->fiber, entry, arg, context->stack, stackBytes, context->asyncStack, asyncBytes);
    return true;
}

void ContextDestroy(Context* context)
{
    RawFree(context->stack);
    RawFree(context->asyncStack);
    context->stack = nullptr;
    context->asyncStack = nullptr;
}

void ContextSwitch(Context* from, Context* to)
{
    emscripten_fiber_swap(&from->fiber, &to->fiber);
}

#else

namespace {
struct Start {
    void (*entry)(void*);
    void* arg;
};

void Trampoline(unsigned high, unsigned low)
{
    const uintptr_t bits = (static_cast<uintptr_t>(high) << 16 << 16) | low;
    auto* start = reinterpret_cast<Start*>(bits);
    void (*entry)(void*) = start->entry;
    void* arg = start->arg;
    delete start;
    entry(arg);
    std::abort();  // a context never returns: the kernel deletes the task instead
}
}  // namespace

void ContextInitMain(Context* context)
{
    getcontext(&context->context);
}

bool ContextCreate(Context* context, void (*entry)(void*), void* arg, std::size_t stackBytes, std::size_t)
{
    context->stack = RawAlloc(stackBytes, 16);
    if (context->stack == nullptr) return false;
    std::memset(context->stack, kStackFill, stackBytes);
    context->stackBytes = stackBytes;
    getcontext(&context->context);
    context->context.uc_stack.ss_sp = context->stack;
    context->context.uc_stack.ss_size = stackBytes;
    context->context.uc_link = nullptr;
    auto* start = new Start{entry, arg};
    const uintptr_t bits = reinterpret_cast<uintptr_t>(start);
    makecontext(&context->context, reinterpret_cast<void (*)()>(Trampoline), 2,
                static_cast<unsigned>(bits >> 16 >> 16), static_cast<unsigned>(bits & 0xFFFFFFFFu));
    return true;
}

void ContextDestroy(Context* context)
{
    RawFree(context->stack);
    context->stack = nullptr;
}

void ContextSwitch(Context* from, Context* to)
{
    swapcontext(&from->context, &to->context);
}

#endif

std::size_t ContextStackUsed(const Context* context)
{
    if (context->stack == nullptr) return 0;
    // Stacks grow down: the untouched fill pattern is at the low end.
    const auto* bytes = static_cast<const uint8_t*>(context->stack);
    std::size_t untouched = 0;
    while (untouched < context->stackBytes && bytes[untouched] == kStackFill) ++untouched;
    return context->stackBytes - untouched;
}

}  // namespace cardenza_web::kernel
