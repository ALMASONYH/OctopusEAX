#pragma once
#include <cstddef>

namespace dsp {

template <class T, size_t N>
class FixedRing {
public:
    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
    T& operator[](size_t i) { return items_[(head_ + i) % N]; }
    const T& operator[](size_t i) const { return items_[(head_ + i) % N]; }
    const T& front() const { return items_[head_]; }
    const T& back() const { return items_[(head_ + count_ - 1) % N]; }

    void clear() { head_ = count_ = 0; }

    void pop_front() {
        if (!count_) return;
        head_ = (head_ + 1) % N;
        --count_;
    }

    void push_back(const T& v) {
        if (count_ == N) pop_front();
        items_[(head_ + count_) % N] = v;
        ++count_;
    }

private:
    T items_[N] = {};
    size_t head_ = 0, count_ = 0;
};

}
