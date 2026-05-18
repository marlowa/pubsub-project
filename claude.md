# Project Rules: C++17 Reactor Framework

## Environment & Tooling
- Host Development OS: Linux Mint 22.2 (Primary focus)
- Host Compiler: GCC 13.3 
- Target Deployment OS: RHEL 8 (GCC 8.5)
- Standard: Strict, portable C++17 only. Absolutely no C++20/C++23 features or modern library additions.

## Local Development Commands
- Build Project: `cmake --build build`
- Run Local Diagnostics: `cmake --build build --target all`

## Architectural Guardrails
- Core pattern is a low-level, event-driven Reactor pattern.
- Sockets must use non-blocking I/O (`O_NONBLOCK`) and edge-triggered `epoll` (`EPOLLET`).
- Do not use modern `std::jthread` or C++20 synchronization primitives (like `std::binary_semaphore`). Stick to C++17 standard features (`std::thread`, `std::mutex`, `std::condition_variable`).
