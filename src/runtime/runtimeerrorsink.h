#pragma once

namespace simlang
{

class TextSink;
struct ExecutableImage;
struct RuntimeError;

class RuntimeErrorSink
{
public:
    virtual void reportRuntimeError(const RuntimeError& error, const ExecutableImage& image) = 0;

protected:
    virtual ~RuntimeErrorSink() = default;
};

class TextRuntimeErrorSink : public RuntimeErrorSink
{
public:
    explicit TextRuntimeErrorSink(TextSink& output)
        : mOutput(output)
    {
    }

    void reportRuntimeError(const RuntimeError& error, const ExecutableImage& image) override;

private:
    TextSink& mOutput;
};

} // namespace simlang
