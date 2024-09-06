#include "yql_suggestion_engine.h"

#include <util/charset/unidata.h>
#include <ydb/public/lib/ydb_cli/commands/interactive/yql_syntax.h>

namespace NYdb {
    namespace NConsoleClient {

        namespace {

            bool IsTokenBoundary(char symbol) {
                static const std::string boundaries = "(.:";
                return IsSpace(symbol) || boundaries.contains(symbol);
            }

            size_t LastTokenBegin(TStringBuf text) {
                const auto length = static_cast<int64_t>(text.size());
                for (int64_t i = length - 1; 0 <= i; --i) {
                    if (IsTokenBoundary(text[i])) {
                        return i + 1;
                    }
                }
                return 0;
            }

            size_t ICaseLongestCommonPrefixLength(TStringBuf lhs, TStringBuf rhs) {
                const auto min_size = std::min(lhs.size(), rhs.size());
                for (size_t i = 0; i < min_size; i++) {
                    if (ToLower(lhs[i]) != ToLower(rhs[i])) {
                        return i;
                    }
                }
                return min_size;
            }

        } // namespace

        YQLSuggestionEngine::YQLSuggestionEngine()
            : Chars()
            , Lexer(&Chars)
            , Tokens(&Lexer)
            , Parser(&Tokens)
            , C3Engine(&Parser)
        {
            Lexer.removeErrorListeners();
            Parser.removeErrorListeners();

            const size_t min = antlr4::Token::MIN_USER_TOKEN_TYPE;
            const size_t max = Lexer.getVocabulary().getMaxTokenType();
            for (size_t token = min; token < max; ++token) {
                bool isVisible = IsYQLKeyword(token) || token == TOKEN(ASTERISK);
                if (!isVisible) {
                    C3Engine.ignoredTokens.emplace(token);
                }
            }
            C3Engine.ignoredTokens.emplace(antlr4::Token::EOF);
        }

        TVector<TString> YQLSuggestionEngine::Candidates(TStringBuf prefix) {
            const auto last_token_begin = LastTokenBegin(prefix);
            const auto last_token_size = prefix.size() - last_token_begin;

            const auto lastToken = prefix.SubString(last_token_begin, last_token_size);

            Reset(prefix.SubString(0, last_token_begin));

            const auto caretTokenIndex = Tokens.size() - 1;
            const auto c3Candidates = C3Engine.collectCandidates(caretTokenIndex);

            TVector<TString> result;

            for (const auto& [token, follow] : c3Candidates.tokens) {
                TString candidate = DisplayName(token);

                const bool isSuitable = //
                    lastToken.Size() < candidate.Size() &&
                    ICaseLongestCommonPrefixLength(lastToken, candidate) == lastToken.Size();

                if (isSuitable) {
                    result.emplace_back(std::move(candidate));
                }
            }

            return result;
        }

        void YQLSuggestionEngine::Reset(TStringBuf prefix) {
            Chars.load(prefix.data(), prefix.length());
            Lexer.reset();
            Tokens.setTokenSource(&Lexer);
            Parser.reset();
            Tokens.fill();
        }

        TString YQLSuggestionEngine::DisplayName(size_t token) const {
            const auto& vocabulary = Lexer.getVocabulary();

            TString display = vocabulary.getDisplayName(token);

            if (display.StartsWith('\'')) {
                assert(display.EndsWith('\''));
                display.erase(std::begin(display));
                display.erase(std::prev(std::end(display)));
            }

            return display;
        }

    } // namespace NConsoleClient
} // namespace NYdb
