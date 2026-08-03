#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "diag/diagnosticlevel.h"
#include "diag/diagnostictype.h"
#include "parser/tokentype.h"
#include "source/sourcerange.h"
#include "symbol/identifier.h"
#include "util/escaping.h"
#include "util/meta.h"
#include "util/types.h"

namespace simlang
{

using DiagnosticParam = std::variant<bool, u64, i64, double, std::string, std::vector<TokenType>>;

struct Diagnostic
{
    template <typename T>
    static DiagnosticParam toParam(T&& value)
    {
        using RawT = std::decay_t<T>;

        if constexpr (std::is_same_v<RawT, DiagnosticParam> || std::is_same_v<RawT, std::string> ||
                      std::is_same_v<RawT, std::vector<TokenType>>)
        {
            // Things that are already in the variant we can directly use.
            return std::forward<T>(value);
        }
        else if constexpr (std::is_same_v<RawT, const char*> || std::is_same_v<RawT, char*> ||
                           std::is_same_v<RawT, std::string_view>)
        {
            // char* goes into std::string (so that we're owning it).
            return std::string{value};
        }
        else if constexpr (std::is_same_v<RawT, char>)
        {
            // For a single char, format it properly.
            // Otherwise we'll get diagnostics like '10' instead of '\n'.
            return escape::formatEscapedByte(static_cast<unsigned char>(value));
        }
        else if constexpr (std::is_pointer_v<RawT> && std::is_same_v<std::remove_pointer_t<RawT>, Identifier>)
        {
            // Identifier* goes into string_view(name, length).
            static std::string cUnknownIdentifier("<unknown identifier>");
            if (value != nullptr)
            {
                return std::string{value->mName, value->mLength};
            }
            return cUnknownIdentifier;
        }
        else if constexpr (std::is_same_v<RawT, TokenType>)
        {
            // Convert tokens to strings, then return that.
            return tokenTypeToString(value);
        }
        else if constexpr (is_std_array_of_token_type<RawT>::value ||
                           std::is_same_v<RawT, std::initializer_list<TokenType>>)
        {
            // Variants and std::arrays aren't exactly friends, so convert to vector.
            return std::vector<TokenType>{value.begin(), value.end()};
        }
        else if constexpr (std::is_same_v<RawT, bool>)
        {
            // Bool remains bool.
            return value;
        }
        else if constexpr (std::is_integral_v<RawT> && std::is_signed_v<RawT>)
        {
            // Signed types go into i64.
            return static_cast<i64>(value);
        }
        else if constexpr (std::is_integral_v<RawT> && std::is_unsigned_v<RawT>)
        {
            // Unsigned types go into u64.
            return static_cast<u64>(value);
        }
        else if constexpr (std::is_floating_point_v<RawT>)
        {
            // Any floating point types go into double.
            return static_cast<double>(value);
        }
        else
        {
            static_assert(always_false_v<T>, "Unsupported type for DiagnosticParam");
        }
    }

    template <typename... Args>
    void addParams(Args&&... args)
    {
        (mParams.push_back(Diagnostic::toParam(std::forward<Args>(args))), ...);
    }

    std::vector<DiagnosticParam> mParams;
    std::string_view mDescription;
    std::string_view mFormat;
    SourceRange mSourceRange = cInvalidSourceRange;
    DiagnosticType mType = cInvalid;
    DiagnosticLevel mLevel = DiagnosticLevel::cInvalid;
    bool mAttached = false;

private:
    // Some SFINAE stuff to match token type arrays.
    template <typename T>
    struct is_std_array_of_token_type : std::false_type
    {
    };

    template <std::size_t N>
    struct is_std_array_of_token_type<std::array<TokenType, N>> : std::true_type
    {
    };
};

} // namespace simlang
