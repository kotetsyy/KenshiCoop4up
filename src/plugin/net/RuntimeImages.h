#ifndef COOP_RUNTIME_IMAGES_H
#define COOP_RUNTIME_IMAGES_H
#include <windows.h>

namespace coop { namespace updater {
namespace detail {
struct RuntimeImageState {
    SRWLOCK lock;
    CONDITION_VARIABLE uiChanged;
    bool uiSettled;
    volatile LONG restartOnly;
};
// External inline linkage is intentional: all core translation units must use
// the same POD state. Constant initialization needs no VC10 static-init lock.
inline RuntimeImageState& runtimeImageState() {
    static RuntimeImageState state = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT, false, 0 };
    return state;
}
}

inline bool runtimeImagesRestartOnly() {
    return InterlockedCompareExchange(&detail::runtimeImageState().restartOnly, 0, 0) != 0;
}

class RuntimeImageLoadGuard {
public:
    explicit RuntimeImageLoadGuard(bool uiLoad = false) : uiLoad_(uiLoad) {
        AcquireSRWLockExclusive(&detail::runtimeImageState().lock);
        allowed_ = !runtimeImagesRestartOnly();
    }
    ~RuntimeImageLoadGuard() {
        detail::RuntimeImageState& state = detail::runtimeImageState();
        if (uiLoad_ && !state.uiSettled) {
            state.uiSettled = true;
            WakeAllConditionVariable(&state.uiChanged);
        }
        ReleaseSRWLockExclusive(&state.lock);
    }
    bool allowed() const { return allowed_; }
private:
    RuntimeImageLoadGuard(const RuntimeImageLoadGuard&);
    RuntimeImageLoadGuard& operator=(const RuntimeImageLoadGuard&);
    bool uiLoad_, allowed_;
};

class RuntimeImageApplyGuard {
public:
    RuntimeImageApplyGuard() : ready_(false) {
        detail::RuntimeImageState& state = detail::runtimeImageState();
        AcquireSRWLockExclusive(&state.lock);
        // Keep the current UI mapped/pinned before any disk replacement. Failed
        // or intentionally omitted first UI attempts settle this condition too.
        while (!state.uiSettled) {
            if (!SleepConditionVariableSRW(&state.uiChanged, &state.lock, INFINITE, 0))
                return;
        }
        if (runtimeImagesRestartOnly()) return;
        InterlockedExchange(&state.restartOnly, 1);
        ready_ = true;
    }
    ~RuntimeImageApplyGuard() {
        ReleaseSRWLockExclusive(&detail::runtimeImageState().lock);
    }
    bool ready() const { return ready_; }
    // Only the caller that verified complete rollback/no recovery artifacts may
    // reopen loads. Success or unresolved rollback remains fenced until exit.
    void allowLoadsAfterRollback() {
        if (ready_) {
            InterlockedExchange(&detail::runtimeImageState().restartOnly, 0);
            ready_ = false;
        }
    }
private:
    RuntimeImageApplyGuard(const RuntimeImageApplyGuard&);
    RuntimeImageApplyGuard& operator=(const RuntimeImageApplyGuard&);
    bool ready_;
};
} }
#endif
