#pragma once
#include "nix/store/build/goal.hh"
#include "nix/store/build/worker.hh"

namespace nix {

template<typename T>
auto Goal::promise_type::await_transform(AsyncCallback<T> && ac)
{
    struct Awaiter
    {
        fun<void(Callback<T>)> fn;
        std::shared_ptr<std::promise<typename Callback<T>::Result>> promise;

        bool await_ready()
        {
            return false;
        }

        void await_suspend(handle_type h)
        {
            auto goal = h.promise().goal;
            promise = std::make_shared<std::promise<typename Callback<T>::Result>>();

            fn(Callback<T>([promise = promise,
                            maybeWaker = goal->worker.getCrossThreadWaker(),
                            goalWeak = goal->weak_from_this()](typename Callback<T>::Result && res) {
                promise->set_value(std::move(res));

                /* The Worker might have already died (and the waker with it) by
                   the time the callback fired. */
                if (auto waker = maybeWaker.lock())
                    waker->enqueue(goalWeak);
            }));

            goal->worker.waitForCompletion(goal->shared_from_this());
        }

        T await_resume()
        {
            return promise->get_future().get().value();
        }
    };

    return Awaiter{.fn = std::move(ac.fn)};
}

} // namespace nix
