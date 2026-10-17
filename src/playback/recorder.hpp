#pragma once
#include <algorithm>
#include <atomic>
#include <vector>
#include "sound_board.hpp"

namespace clip {

class Recorder {
public:
    Recorder() : ring_(RING) {}

    void start() {
        r_.store(w_.load(std::memory_order_acquire), std::memory_order_release);
        on_.store(true, std::memory_order_release);
    }

    void stop() { on_.store(false, std::memory_order_release); }

    bool on() const { return on_.load(std::memory_order_acquire); }

    void feed(const float* lr, size_t frames) {
        if (!on_.load(std::memory_order_acquire)) return;
        const size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
        const size_t want = frames * 2, n = std::min(want, RING - (w - r));
        for (size_t i = 0; i < n; ++i) ring_[(w + i) & MASK] = toShort(lr[i]);
        w_.store(w + n, std::memory_order_release);
    }

    void drain(std::vector<short>& out, size_t most) {
        const size_t w = w_.load(std::memory_order_acquire), r = r_.load(std::memory_order_relaxed);
        const size_t room = most > out.size() ? most - out.size() : 0;
        const size_t n = std::min(w - r, room) & ~(size_t)1;
        for (size_t i = 0; i < n; ++i) out.push_back(ring_[(r + i) & MASK]);
        r_.store(r + n, std::memory_order_release);
    }

private:
    static const size_t RING = (size_t)1 << 20, MASK = RING - 1;
    std::vector<short> ring_;
    std::atomic<size_t> w_{0}, r_{0};
    std::atomic<bool> on_{false};
};

class Backlog {
public:
    size_t capacity() const { return ring_.size() / 2; }
    size_t frames() const { return fill_; }

    void clear() { head_ = fill_ = 0; }

    void resize(size_t frames) {
        std::vector<short> next(frames * 2);
        const size_t keep = std::min(frames, fill_);
        copyLast(keep, next.data());
        ring_.swap(next);
        fill_ = keep;
        head_ = frames ? keep % frames : 0;
    }

    void push(const short* lr, size_t frames) {
        const size_t cap = capacity();
        if (!cap || !frames) return;
        if (frames > cap) {
            lr += (frames - cap) * 2;
            frames = cap;
        }
        for (size_t done = 0; done < frames;) {
            const size_t n = std::min(frames - done, cap - head_);
            std::copy(lr + done * 2, lr + (done + n) * 2, ring_.begin() + (std::ptrdiff_t)(head_ * 2));
            head_ = (head_ + n) % cap;
            done += n;
        }
        fill_ = std::min(cap, fill_ + frames);
    }

    std::vector<short> last(size_t frames) const {
        frames = std::min(frames, fill_);
        std::vector<short> out(frames * 2);
        copyLast(frames, out.data());
        return out;
    }

private:
    void copyLast(size_t frames, short* to) const {
        const size_t cap = capacity();
        if (!frames || !cap) return;
        const size_t from = (head_ + cap - frames) % cap, first = std::min(frames, cap - from);
        std::copy(ring_.begin() + (std::ptrdiff_t)(from * 2), ring_.begin() + (std::ptrdiff_t)((from + first) * 2), to);
        std::copy(ring_.begin(), ring_.begin() + (std::ptrdiff_t)((frames - first) * 2), to + first * 2);
    }

    std::vector<short> ring_;
    size_t head_ = 0, fill_ = 0;
};

}
