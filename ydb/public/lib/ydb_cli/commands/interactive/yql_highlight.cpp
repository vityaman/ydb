#include "yql_highlight.h"

#include "yql_syntax.h"

#include <ydb/public/lib/ydb_cli/commands/interactive/yql_position.h>

#include <yql/essentials/public/issue/yql_issue.h>
#include <yql/essentials/sql/settings/translation_settings.h>
#include <yql/essentials/sql/v1/lexer/lexer.h>

#include <util/charset/utf8.h>
#include <util/string/strip.h>

#include <regex>
#include <yql/essentials/parser/proto_ast/gen/v1_antlr4/SQLv1Antlr4Lexer.h>

#define TOKEN(NAME) SQLv1Antlr4Lexer::TOKEN_##NAME

namespace NYdb {
    namespace NConsoleClient {
        using NSQLTranslation::SQL_MAX_PARSER_ERRORS;
        using NSQLTranslationV1::IsProbablyKeyword;
        using NSQLTranslationV1::MakeLexer;
        using NYql::TIssues;

        using std::regex_constants::ECMAScript;
        using std::regex_constants::icase;

        constexpr const char* builtinFunctionPattern = ( //
            "^("
            "abs|aggregate_by|aggregate_list|aggregate_list_distinct|agg_list|agg_list_distinct|"
            "as_table|avg|avg_if|adaptivedistancehistogram|adaptivewardhistogram|adaptiveweighthistogram|"
            "addmember|addtimezone|aggregateflatten|aggregatetransforminput|aggregatetransformoutput|"
            "aggregationfactory|asatom|asdict|asdictstrict|asenum|aslist|asliststrict|asset|assetstrict|"
            "asstruct|astagged|astuple|asvariant|atomcode|bitcast|bit_and|bit_or|bit_xor|bool_and|"
            "bool_or|bool_xor|bottom|bottom_by|blockwardhistogram|blockweighthistogram|cast|coalesce|"
            "concat|concat_strict|correlation|count|count_if|covariance|covariance_population|"
            "covariance_sample|callableargument|callableargumenttype|callableresulttype|callabletype|"
            "callabletypecomponents|callabletypehandle|choosemembers|combinemembers|countdistinctestimate|"
            "currentauthenticateduser|currentoperationid|currentoperationsharedid|currenttzdate|"
            "currenttzdatetime|currenttztimestamp|currentutcdate|currentutcdatetime|currentutctimestamp|"
            "dense_rank|datatype|datatypecomponents|datatypehandle|dictaggregate|dictcontains|dictcreate|"
            "dicthasitems|dictitems|dictkeytype|dictkeys|dictlength|dictlookup|dictpayloadtype|dictpayloads|"
            "dicttype|dicttypecomponents|dicttypehandle|each|each_strict|emptydicttype|emptydicttypehandle|"
            "emptylisttype|emptylisttypehandle|endswith|ensure|ensureconvertibleto|ensuretype|enum|"
            "evaluateatom|evaluatecode|evaluateexpr|evaluatetype|expandstruct|filter|filter_strict|find|"
            "first_value|folder|filecontent|filepath|flattenmembers|forceremovemember|forceremovemembers|"
            "forcerenamemembers|forcespreadmembers|formatcode|formattype|frombytes|frompg|funccode|"
            "greatest|grouping|gathermembers|generictype|histogram|hll|hoppingwindowpgcast|hyperloglog|"
            "if|if_strict|instanceof|json_exists|json_query|json_value|jointablerow|just|lag|last_value|"
            "lead|least|len|length|like|likely|like_strict|lambdaargumentscount|lambdacode|"
            "lambdaoptionalargumentscount|linearhistogram|listaggregate|listall|listany|listavg|listcode|"
            "listcollect|listconcat|listcreate|listdistinct|listenumerate|listextend|listextendstrict|"
            "listextract|listfilter|listflatmap|listflatten|listfold|listfold1|listfold1map|listfoldmap|"
            "listfromrange|listfromtuple|listhas|listhasitems|listhead|listindexof|listitemtype|listlast|"
            "listlength|listmap|listmax|listmin|listnotnull|listreplicate|listreverse|listskip|"
            "listskipwhile|listskipwhileinclusive|listsort|listsortasc|listsortdesc|listsum|listtake|"
            "listtakewhile|listtakewhileinclusive|listtotuple|listtype|listtypehandle|listunionall|"
            "listuniq|listzip|listzipall|loghistogram|logarithmichistogram|max|max_by|max_of|median|"
            "min|min_by|min_of|mode|multi_aggregate_by|nanvl|nvl|nothing|nulltype|nulltypehandle|"
            "optionalitemtype|optionaltype|optionaltypehandle|percentile|parsefile|parsetype|"
            "parsetypehandle|pgand|pgarray|pgcall|pgconst|pgnot|pgop|pgor|pickle|quotecode|range|"
            "range_strict|rank|regexp|regexp_strict|rfind|row_number|random|randomnumber|randomuuid|"
            "removemember|removemembers|removetimezone|renamemembers|replacemember|reprcode|resourcetype|"
            "resourcetypehandle|resourcetypetag|some|stddev|stddev_population|stddev_sample|substring|"
            "sum|sum_if|sessionstart|sessionwindow|setcreate|setdifference|setincludes|setintersection|"
            "setisdisjoint|setsymmetricdifference|setunion|spreadmembers|stablepickle|startswith|staticmap|"
            "staticzip|streamitemtype|streamtype|streamtypehandle|structmembertype|structmembers|"
            "structtypecomponents|structtypehandle|subqueryextend|subqueryextendfor|subquerymerge|"
            "subquerymergefor|subqueryunionall|subqueryunionallfor|subqueryunionmerge|subqueryunionmergefor"
            "|top|topfreq|top_by|tablename|tablepath|tablerecordindex|tablerow|tablerows|taggedtype|"
            "taggedtypecomponents|taggedtypehandle|tobytes|todict|tomultidict|topg|toset|tosorteddict|"
            "tosortedmultidict|trymember|tupleelementtype|tupletype|tupletypecomponents|tupletypehandle|"
            "typehandle|typekind|typeof|udaf|udf|unittype|unpickle|untag|unwrap|variance|variance_population|"
            "variance_sample|variant|varianttype|varianttypehandle|variantunderlyingtype|voidtype|"
            "voidtypehandle|way|worldcode|weakfield"
            ")$");

        constexpr const char* typePattern = ( //
            "^("
            "bool|date|datetime|decimal|double|float|int16|int32|int64|int8|interval|json|jsondocument|"
            "string|timestamp|tzdate|tzdatetime|tztimestamp|uint16|uint32|uint64|uint8|utf8|uuid|yson|"
            "text|bytes"
            ")$");

        YQLHighlight::ColorSchema YQLHighlight::ColorSchema::Monaco() {
            using replxx::color::rgb666;

            return {
                .keyword = Color::BLUE,
                .operation = rgb666(3, 3, 3),
                .identifier = {
                    .function = rgb666(4, 1, 5),
                    .type = rgb666(2, 3, 2),
                    .variable = Color::DEFAULT,
                    .quoted = rgb666(1, 3, 3),
                },
                .string = rgb666(3, 0, 0),
                .number = Color::BRIGHTGREEN,
                .comment = rgb666(2, 2, 2),
                .unknown = Color::DEFAULT,
            };
        }

        YQLHighlight::ColorSchema YQLHighlight::ColorSchema::Debug() {
            using replxx::color::rgb666;

            return {
                .keyword = Color::BLUE,
                .operation = Color::GRAY,
                .identifier = {
                    .function = Color::MAGENTA,
                    .type = Color::YELLOW,
                    .variable = Color::RED,
                    .quoted = Color::CYAN,
                },
                .string = Color::GREEN,
                .number = Color::BRIGHTGREEN,
                .comment = rgb666(2, 2, 2),
                .unknown = Color::DEFAULT,
            };
        }

        YQLHighlight::YQLHighlight(ColorSchema color)
            : Coloring(color)
            , BuiltinFunctionRegex(builtinFunctionPattern, ECMAScript | icase)
            , TypeRegex(typePattern, ECMAScript | icase)
            , CppLexer(MakeLexer(/* ansi = */ false, /* antlr4 = */ true))
            , ANSILexer(MakeLexer(/* ansi = */ true, /* antlr4 = */ true))
        {
        }

        void YQLHighlight::Apply(TStringBuf queryUtf8, Colors& colors) {
            Tokens = Tokenize(TString(queryUtf8));
            auto mapping = YQLPositionMapping::Build(TString(queryUtf8));

            for (std::size_t i = 0; i < Tokens.size() - 1; ++i) {
                const auto& token = Tokens.at(i);
                const auto color = ColorOf(token, i);

                const std::ptrdiff_t start = mapping.RawPos(token);
                const std::ptrdiff_t length = GetNumberOfUTF8Chars(token.Content);
                const std::ptrdiff_t stop = start + length;

                std::fill(std::next(std::begin(colors), start),
                          std::next(std::begin(colors), stop), color);
            }
        }

        TParsedTokenList YQLHighlight::Tokenize(const TString& queryUtf8) {
            ILexer& lexer = *SuitableLexer(queryUtf8);

            TParsedTokenList tokens;
            TIssues issues;
            NSQLTranslation::Tokenize(lexer, queryUtf8, "Query", tokens, issues, SQL_MAX_PARSER_ERRORS);
            return tokens;
        }

        ILexer::TPtr& YQLHighlight::SuitableLexer(const TString& queryUtf8) {
            if (IsAnsiQuery(queryUtf8)) {
                return ANSILexer;
            }
            return CppLexer;
        }

        YQLHighlight::Color YQLHighlight::ColorOf(const TParsedToken& token, size_t index) {
            if (IsString(token)) {
                return Coloring.string;
            }
            if (IsFunctionIdentifier(token, index)) {
                return Coloring.identifier.function;
            }
            if (IsTypeIdentifier(token)) {
                return Coloring.identifier.type;
            }
            if (IsVariableIdentifier(token)) {
                return Coloring.identifier.variable;
            }
            if (IsQuotedIdentifier(token)) {
                return Coloring.identifier.quoted;
            }
            if (IsNumber(token)) {
                return Coloring.number;
            }
            if (IsOperation(token)) {
                return Coloring.operation;
            }
            if (IsKeyword(token)) {
                return Coloring.keyword;
            }
            if (IsComment(token)) {
                return Coloring.comment;
            }
            return Coloring.unknown;
        }

        bool YQLHighlight::IsKeyword(const TParsedToken& token) const {
            return IsProbablyKeyword(token);
        }

        bool YQLHighlight::IsOperation(const TParsedToken& token) const {
            return IsOperationTokenName(token.Name);
        }

        bool YQLHighlight::IsFunctionIdentifier(const TParsedToken& token, size_t index) {
            if (!IsPlainIdentifierTokenName(token.Name)) {
                return false;
            }
            return std::regex_search(token.Content.begin(), token.Content.end(), BuiltinFunctionRegex) ||
                   (2 <= index && IsNamespaceTokenName(Tokens.at(index - 1).Name) && IsPlainIdentifierTokenName(Tokens.at(index - 2).Name)) ||
                   (index < Tokens.size() - 1 && IsNamespaceTokenName(Tokens.at(index + 1).Name));
        }

        bool YQLHighlight::IsTypeIdentifier(const TParsedToken& token) const {
            return IsPlainIdentifierTokenName(token.Name) && std::regex_search(token.Content.begin(), token.Content.end(), TypeRegex);
        }

        bool YQLHighlight::IsVariableIdentifier(const TParsedToken& token) const {
            return IsPlainIdentifierTokenName(token.Name);
        }

        bool YQLHighlight::IsQuotedIdentifier(const TParsedToken& token) const {
            return IsQuotedIdentifierTokenName(token.Name);
        }

        bool YQLHighlight::IsString(const TParsedToken& token) const {
            return IsStringTokenName(token.Name);
        }

        bool YQLHighlight::IsNumber(const TParsedToken& token) const {
            return IsNumberTokenName(token.Name);
        }

        bool YQLHighlight::IsComment(const TParsedToken& token) const {
            return IsCommentTokenName(token.Name);
        }

    } // namespace NConsoleClient
} // namespace NYdb
