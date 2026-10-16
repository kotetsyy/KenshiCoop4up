#ifndef KENSHICOOP_NATIVE_LOCK_H
#define KENSHICOOP_NATIVE_LOCK_H
#include <windows.h>
namespace coop {
// Win32 critical sections are recursive; scoped release also covers allocation
// failures in the v100 queues, so an explicit failed session cannot deadlock MAIN.
class NativeLock {
public:
    explicit NativeLock(CRITICAL_SECTION* cs):cs_(cs) {EnterCriticalSection(cs_);}
    ~NativeLock() {unlock();}
    void unlock() {if(cs_){LeaveCriticalSection(cs_);cs_=0;}}
private:
    CRITICAL_SECTION* cs_;
    NativeLock(const NativeLock&);
    NativeLock& operator=(const NativeLock&);
};
}
#endif
