#pragma once

#include <contrib/libs/antlr4_cpp_runtime/src/Token.h>
#include <contrib/libs/antlr4_cpp_runtime/src/BufferedTokenStream.h>
#include <contrib/libs/antlr4_cpp_runtime/src/ANTLRInputStream.h>

#include <ydb/public/lib/ydb_cli/commands/interactive/antlr4-c3/CodeCompletionCore.hpp>

#include <ydb/library/yql/parser/proto_ast/gen/v1_antlr4/SQLv1Antlr4Lexer.h>
#include <ydb/library/yql/parser/proto_ast/gen/v1_antlr4/SQLv1Antlr4Parser.h>

#include <util/generic/fwd.h>

namespace NYdb {
    namespace NConsoleClient {

        using NALPDefaultAntlr4::SQLv1Antlr4Lexer;
        using NALPDefaultAntlr4::SQLv1Antlr4Parser;

        class YQLSuggestionEngine final {
        public:
            explicit YQLSuggestionEngine();

        public:
            TVector<TString> Candidates(TStringBuf prefix);

        private:
            void Reset(TStringBuf prefix);

            TString DisplayName(size_t token) const;

        private:
            antlr4::ANTLRInputStream Chars;
            NALPDefaultAntlr4::SQLv1Antlr4Lexer Lexer;
            antlr4::BufferedTokenStream Tokens;
            NALPDefaultAntlr4::SQLv1Antlr4Parser Parser;
            c3::CodeCompletionCore C3Engine;
        };

    }
}
