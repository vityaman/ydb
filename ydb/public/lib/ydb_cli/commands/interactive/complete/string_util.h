#pragma once

#include <util/charset/unidata.h>

#include <string_view>

namespace NSQLComplete {

    static constexpr std::string_view WordBreakCharacters = " \t\v\f\a\b\r\n`~!@#$%^&*-=+[](){}\\|;:'\".,<>/?";

    bool IsWordBoundary(char ch);

    size_t LastWordIndex(TStringBuf text);

    TStringBuf LastWord(TStringBuf text);

} // namespace NSQLComplete
