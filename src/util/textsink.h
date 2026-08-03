#pragma once

#include <string_view>

namespace simlang
{

class TextSink
{
public:
    virtual ~TextSink() = default;

    virtual void write(std::string_view text) = 0;
    virtual void flush() {}
};

} // namespace simlang
