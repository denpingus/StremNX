#pragma once

#include <memory>
#include <mutex>
#include <utility>

#include "thread.hpp"

// Runs a function on a background thread without blocking the UI.
// If the screen is closed before it finishes, the result is simply discarded.
template <class T>
class Task {
    struct State {
        std::mutex m;
        bool done = false;
        T value;
    };
    std::shared_ptr<State> st;

public:
    template <class F>
    void start(F fn) {
        auto s = std::make_shared<State>();
        st = s;
        bool ok = startThread([s, fn]() mutable {
            T v = fn();
            std::lock_guard<std::mutex> lock(s->m);
            s->value = std::move(v);
            s->done = true;
        });
        if (!ok) {  // no thread could be made (memory is tight): do the work here rather than never finishing
            T v = fn();
            std::lock_guard<std::mutex> lock(s->m);
            s->value = std::move(v);
            s->done = true;
        }
    }

    // Started and not yet consumed with take().
    bool active() const { return (bool)st; }

    bool ready() const {
        if (!st) return false;
        std::lock_guard<std::mutex> lock(st->m);
        return st->done;
    }

    T take() {
        T v;
        {
            std::lock_guard<std::mutex> lock(st->m);
            v = std::move(st->value);
        }
        st.reset();
        return v;
    }

    void cancel() { st.reset(); }
};
