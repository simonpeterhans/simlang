#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "util/textsink.h"

namespace simlang
{

class FileTextSink : public TextSink
{
public:
    explicit FileTextSink(std::FILE* file)
        : mFile(file)
    {
    }

    void write(std::string_view text) override
    {
        // Our sink is a file that we can write to (but don't own).
        if (mFile == nullptr || text.empty())
        {
            return;
        }

        std::fwrite(text.data(), 1, text.size(), mFile);
    }

    void flush() override
    {
        if (mFile != nullptr)
        {
            std::fflush(mFile);
        }
    }

private:
    std::FILE* mFile = nullptr;
};

class OwnedFileTextSink : public TextSink
{
public:
    explicit OwnedFileTextSink(std::FILE* file)
        : mFile(file)
    {
    }

    ~OwnedFileTextSink() override
    {
        // If we go out of scope, make sure the file is closed.
        if (mFile != nullptr)
        {
            std::fclose(mFile);
        }
    }

    OwnedFileTextSink(const OwnedFileTextSink&) = delete;
    OwnedFileTextSink& operator=(const OwnedFileTextSink&) = delete;

    void write(std::string_view text) override
    {
        // Our sink is a file that we can write to (and own).
        if (mFile == nullptr || text.empty())
        {
            return;
        }

        std::fwrite(text.data(), 1, text.size(), mFile);
    }

    void flush() override
    {
        if (mFile != nullptr)
        {
            std::fflush(mFile);
        }
    }

private:
    std::FILE* mFile = nullptr;
};

class StringTextSink : public TextSink
{
public:
    void write(std::string_view text) override
    {
        // This is the simplest one, our sink is just a string.
        mText.append(text);
    }

    const std::string& getText() const { return mText; }

    std::string takeText() { return std::move(mText); }

    void clear() { mText.clear(); }

private:
    std::string mText;
};

} // namespace simlang
