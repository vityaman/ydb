#include <library/cpp/testing/unittest/registar.h>

#include "sql_format.h"
#include <yql/essentials/sql/v1/lexer/antlr3/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr3_ansi/lexer.h>
#include <yql/essentials/sql/v1/proto_parser/antlr3/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr3_ansi/proto_parser.h>

#include <google/protobuf/arena.h>
#include <util/string/subst.h>
#include <util/string/join.h>

namespace {

using TCases = TVector<std::pair<TString, TString>>;

struct TSetup {
    TSetup(bool ansiLexer = false) {
        NSQLTranslationV1::TLexers lexers;
        lexers.Antlr3 = NSQLTranslationV1::MakeAntlr3LexerFactory();
        lexers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiLexerFactory();
        NSQLTranslationV1::TParsers parsers;
        parsers.Antlr3 = NSQLTranslationV1::MakeAntlr3ParserFactory();
        parsers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiParserFactory();

        NSQLTranslation::TTranslationSettings settings;
        settings.Antlr4Parser = false;
        settings.Arena = &Arena;
        settings.AnsiLexer = ansiLexer;
        Formatter = NSQLFormat::MakeSqlFormatter(lexers, parsers, settings);
    }

    void Run(const TCases& cases, NSQLFormat::EFormatMode mode = NSQLFormat::EFormatMode::Pretty) {
        NSQLTranslationV1::TLexers lexers;
        lexers.Antlr3 = NSQLTranslationV1::MakeAntlr3LexerFactory();
        lexers.Antlr3Ansi = NSQLTranslationV1::MakeAntlr3AnsiLexerFactory();

        for (const auto& c : cases) {
            NYql::TIssues issues;
            TString formatted;
            auto res = Formatter->Format(c.first, formatted, issues, mode);
            UNIT_ASSERT_C(res, issues.ToString());
            auto expected = c.second;
            SubstGlobal(expected, "\t", TString(NSQLFormat::OneIndent, ' '));
            UNIT_ASSERT_NO_DIFF(formatted, expected);

            TString formatted2;
            auto res2 = Formatter->Format(formatted, formatted2, issues);
            UNIT_ASSERT_C(res2, issues.ToString());
            UNIT_ASSERT_NO_DIFF(formatted, formatted2);


            if (mode == NSQLFormat::EFormatMode::Pretty) {
                NSQLTranslation::TTranslationSettings settings;
                settings.Antlr4Parser = false;
                auto mutatedQuery = NSQLFormat::MutateQuery(lexers, c.first, settings);
                auto res3 = Formatter->Format(mutatedQuery, formatted, issues);
                UNIT_ASSERT_C(res3, issues.ToString());
            }
        }
    }

    google::protobuf::Arena Arena;
    NSQLFormat::ISqlFormatter::TPtr Formatter;
};

}

Y_UNIT_TEST_SUITE(CheckSqlFormatter) {
    #include "sql_format_ut.h"
}
