#pragma once

// One execution context per FreeRTOS task. On the web it is an Emscripten fiber (Asyncify: the
// task's wasm call stack is unwound into its own buffer when it gives up the CPU and rewound when it
// gets it back); in the host tests it is a ucontext. Everything above this file is the same code.

#include <cstddef>
#include <cstdint>

#if defined(__EMSCRIPTEN__)
#include <emscripten/fiber.h>
#else
#include <ucontext.h>
#endif

namespace cardenza_web::kernel {

struct Context {
#if defined(__EMSCRIPTEN__)
    emscripten_fiber_t fiber{};
#else
    ucontext_t context{};
#endif
    void* stack = nullptr;        // C stack (linear memory)
    std::size_t stackBytes = 0;
    void* asyncStack = nullptr;   // Asyncify's buffer for the unwound call stack (web only)
    std::size_t asyncBytes = 0;
};

// The context the scheduler runs on (the page's call into cardenza_web_run). Once.
void ContextInitMain(Context* context);
// A new context that calls entry(arg) the first time it is switched to.
bool ContextCreate(Context* context, void (*entry)(void*), void* arg, std::size_t stackBytes, std::size_t asyncBytes);
void ContextDestroy(Context* context);
// Saves the running context into `from` and continues `to`; returns when something switches back.
void ContextSwitch(Context* from, Context* to);
// Lowest address of the C stack in use and how much of it was ever touched (for high-water marks).
std::size_t ContextStackUsed(const Context* context);

}  // namespace cardenza_web::kernel
