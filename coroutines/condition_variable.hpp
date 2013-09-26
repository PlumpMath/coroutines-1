#ifndef CONDITION_VARIABLE_HPP
#define CONDITION_VARIABLE_HPP

#include "monitor.hpp"

namespace coroutines {

// coroutine version of condition variables.
// partially source-compatible with std::consdition_variable_any
class condition_variable
{
public:

    void notify_all()
    {
        _monitor.wake_all();
    }

    template<typename Lock>
    void wait(Lock& lock);

    template<typename Lock, typename Predicate>
    void wait(Lock& lock, Predicate pred)
    {
        while(!pred())
            wait(lock);
    }

private:

    monitor _monitor;

};


template<typename Lock>
void condition_variable::wait(Lock& lock)
{
    lock.unlock();
    _monitor.wait();
    lock.lock();
    // TODO is this correct? maybe the lock should be released only in coroutine's epilogue?
}

}

#endif // CONDITION_VARIABLE_HPP
