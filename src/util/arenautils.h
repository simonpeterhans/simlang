#pragma once

#include <vector>

#include "util/arena.h"
#include "util/arrayview.h"

namespace simlang
{

template <typename T>
ArrayView<T> makeArrayView(ArenaAllocator& alloc, const std::vector<T>& src)
{
    if (src.empty())
    {
        return ArrayView<T>{nullptr, 0};
    }

    T* data = alloc.createArray<T>(src.size());
    for (usize i = 0; i < src.size(); ++i)
    {
        data[i] = src[i];
    }

    return ArrayView<T>{data, src.size()};
}

} // namespace simlang
