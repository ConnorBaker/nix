/// blocking-scope.cc — BlockingThreadPool implementation.

#include "blocking-scope.hh"

namespace nix::eval_trace {

BlockingThreadPool::BlockingThreadPool(boost::asio::io_context & /* ioc — structural dependency */, uint32_t numThreads)
{
    for (uint32_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::unique_ptr<WorkBase> task;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [this] { return stopping_ || !work_.empty(); });
                    if (stopping_ && work_.empty())
                        return;
                    task = std::move(work_.front());
                    work_.erase(work_.begin());
                }
                (*task)();
            }
        });
    }
}

BlockingThreadPool::~BlockingThreadPool()
{
    stop();
}

void BlockingThreadPool::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto & t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
}

} // namespace nix::eval_trace
