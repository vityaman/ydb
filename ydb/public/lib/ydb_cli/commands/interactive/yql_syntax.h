#pragma once

#include <cstddef>

#define TOKEN(NAME) NALPDefaultAntlr4::SQLv1Antlr4Lexer::TOKEN_##NAME

namespace NYdb {
    namespace NConsoleClient {

        bool IsYQLKeyword(size_t token_type);

    }
}
