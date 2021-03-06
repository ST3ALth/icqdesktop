#include "stdafx.h"
#include "threadpool.h"
#include "../../common.shared/crash_handler.h"

using namespace core;
using namespace tools;

#ifdef __linux__
#include <signal.h>
#endif //__linux__

threadpool::threadpool(const unsigned count, std::function<void()> _on_thread_exit)
    :	stop_(false)
{
    creator_thread_id_ = boost::this_thread::get_id();

    threads_.reserve(count);

    const auto worker = [this, _on_thread_exit]
    {
        for(;;)
        {
            if (!run_task())
                break;
        }

        _on_thread_exit();
    };

    for (unsigned i = 0; i < count; i++)
    {
        threads_.emplace_back(worker);
        threads_ids_.emplace_back(threads_[i].get_id());
    }
}

bool threadpool::run_task_impl()
{
    task nextTask;

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        condition_.wait(lock, [this]
        {
            return stop_ || !tasks_.empty();
        });

        if (stop_ && tasks_.empty())
        {
            return false;
        }

        nextTask = std::move(tasks_.front());
        tasks_.pop();
    }

    nextTask();
    return true;
}

bool threadpool::run_task()
{
#ifdef _WIN32
     __try
#endif // _WIN32
    {
         return run_task_impl();
    }

#ifdef _WIN32
    __except(::core::dump::crash_handler::seh_handler(GetExceptionInformation()))
    {
    }
#endif // _WIN32
    return true;
}

threadpool::~threadpool()
{
    if (creator_thread_id_ != boost::this_thread::get_id())
    {
        assert(!"invalid destroy thread");
    }

    stop_ = true;

    condition_.notify_all();

    for (auto &worker: threads_)
    {
        worker.join();
    }

}

bool threadpool::enqueue(const task _task)
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_)
        {
            return false;
        }
#ifdef __linux__
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
            assert(false);
#endif //__linux__

#ifdef _WIN32
        tasks_.emplace([_task]
        {
            core::dump::crash_handler handler;
            handler.set_product_bundle("icq.desktop");
            handler.set_thread_exception_handlers();
            _task();
        });
#else
        tasks_.emplace(_task);
#endif // _WIN32
    }

    condition_.notify_one();

    return true;
}

const std::vector<std::thread::id> threadpool::get_threads_ids() const
{
    return threads_ids_;
}
