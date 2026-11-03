#pragma once

#include <pico/critical_section.h>

/**
 * Cross-core lock that also masks IRQs on the holding core.
 * lwIP/AsyncTCP callbacks run in a core0 IRQ: a pico mutex held by the core0 loop would
 * deadlock that IRQ (or, if recursive, let it in mid-update). Keep sections short.
 */
class IrqSafeLock
{
public:
    IrqSafeLock() { critical_section_init(&section); }
    IrqSafeLock(const IrqSafeLock &) = delete;
    IrqSafeLock &operator=(const IrqSafeLock &) = delete;

    void lock() { critical_section_enter_blocking(&section); }
    void unlock() { critical_section_exit(&section); }

private:
    critical_section_t section{};
};

class IrqSafeGuard
{
public:
    explicit IrqSafeGuard(IrqSafeLock &lock) : lock(lock) { lock.lock(); }
    ~IrqSafeGuard() { lock.unlock(); }
    IrqSafeGuard(const IrqSafeGuard &) = delete;
    IrqSafeGuard &operator=(const IrqSafeGuard &) = delete;

private:
    IrqSafeLock &lock;
};
