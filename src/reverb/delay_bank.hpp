#pragma once
#include <cstddef>
#include <vector>
#include "reverb_params.hpp"

namespace eax {

inline int nextPow2(int n) {
    int p = 2;
    while (p < n) p <<= 1;
    return p;
}

struct Line4 {
    std::vector<double> buf;
    int size, mask;
    void init(int maxlen) {
        size = nextPow2(maxlen + 264);
        mask = size - 1;
        buf.assign((size_t)size * NUM_LINES, 0.0);
    }
    inline double* ch(int j) { return buf.data() + (size_t)j * size; }
};

}
