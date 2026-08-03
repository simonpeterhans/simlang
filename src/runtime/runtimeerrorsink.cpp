#include "runtime/runtimeerrorsink.h"

#include <string_view>

#include "runtime/executableimage.h"
#include "runtime/runtimedebuginfo.h"
#include "runtime/runtimeerror.h"
#include "runtime/vmdefines.h"
#include "util/textwriter.h"
#include "util/types.h"

namespace simlang
{

void TextRuntimeErrorSink::reportRuntimeError(const RuntimeError& error, const ExecutableImage& image)
{
    TextWriter out{mOutput};

    out << "[runtime] error";

    bool wroteLocation = false;
    if (error.mSourceLocation != nullptr)
    {
        // If we have a source location for this error, print it.
        const RuntimeSourceLocation& location = *error.mSourceLocation;
        if (location.isValid())
        {
            wroteLocation = true;
            std::string_view filename = image.mDebugInfo.getSourceFilename(location.mSourceID);

            out << " at ";
            if (filename.empty())
            {
                out << "<unknown>";
            }
            else
            {
                out << filename;
            }
            out << ':' << location.mLine << ':' << location.mColumn;
        }
    }

    if (wroteLocation == false && error.mBytecodeAddress != cInvalidVMAddress)
    {
        // Otherwise, if we have a bytecode address, write that instead.
        out << " at bytecode " << error.mBytecodeAddress;
    }
    out << ": " << runtimeErrorKindToString(error.mKind);

    switch (error.mKind)
    {
        case RuntimeErrorKind::cStackOverflow:
        {
            out << " (required " << error.mValue0 << " word(s), limit " << error.mValue1 << " word(s))";
            break;
        }
        case RuntimeErrorKind::cIndexOutOfBounds:
        {
            out << " (index " << error.mValue0 << ", length " << error.mValue1 << ")";
            break;
        }
        case RuntimeErrorKind::cNegativeListCapacity:
        case RuntimeErrorKind::cNegativeMapCapacity:
        {
            out << " (" << error.mValue0 << ")";
            break;
        }
        case RuntimeErrorKind::cAllocationFailed:
        {
            out << " (requested " << error.mValue0 << ")";
            break;
        }
        case RuntimeErrorKind::cInvalidReference:
        {
            out << " (address " << static_cast<u64>(error.mValue0) << ")";
            break;
        }
        case RuntimeErrorKind::cInvalidStringHandle:
        {
            if (error.mValue0 == static_cast<i64>(cNullRef))
            {
                out << " (<null>)";
            }
            else
            {
                out << " (handle " << static_cast<u64>(error.mValue0) << ")";
            }
            break;
        }
        case RuntimeErrorKind::cSyscallFailed:
        {
            out << " (syscall " << error.mValue0 << ")";
            break;
        }
        case RuntimeErrorKind::cInvalidCast:
        {
            if (error.mValue0 != 0 || error.mValue1 != 0)
            {
                out << " (actual type " << error.mValue0 << ", target type " << error.mValue1 << ")";
            }
            break;
        }
        case RuntimeErrorKind::cShiftOutOfRange:
        {
            out << " (" << error.mValue0 << ")";
            break;
        }
        default:
        {
            break;
        }
    }

    out << '\n';
}

} // namespace simlang
