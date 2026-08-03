#pragma once

#include <string>
#include <string_view>

#include "util/types.h"

namespace simlang::escape
{

bool decodeEscapeSequence(std::string_view text, usize& charsConsumed, char& out);
bool validateEscapeSequence(std::string_view text, usize& charsConsumed);
std::string formatEscapedByte(unsigned char value);
std::string formatEscapedText(std::string_view text);

} // namespace simlang::escape
