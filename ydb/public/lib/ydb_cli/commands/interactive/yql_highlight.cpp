#include "yql_highlight.h"

#include <ydb/public/lib/ydb_cli/commands/interactive/yql_syntax.h>

#include <contrib/libs/antlr4_cpp_runtime/src/antlr4-runtime.h>
#include <regex>

#include <ydb/library/yql/parser/proto_ast/gen/v1_antlr4/SQLv1Antlr4Lexer.h>

namespace NYdb {
    namespace NConsoleClient {

        using NALPDefaultAntlr4::SQLv1Antlr4Lexer;

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
                .unknown = Color::DEFAULT,
            };
        }

        YQLHighlight::ColorSchema YQLHighlight::ColorSchema::Debug() {
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
                .unknown = Color::DEFAULT,
            };
        }

        YQLHighlight::YQLHighlight(ColorSchema color)
            : Coloring(color)
            , BuiltinFunctionRegex(builtinFunctionPattern, std::regex_constants::ECMAScript | std::regex_constants::icase)
            , TypeRegex(typePattern, std::regex_constants::ECMAScript | std::regex_constants::icase)
            , Chars()
            , Lexer(&Chars)
            , Tokens(&Lexer)
        {
            Lexer.removeErrorListeners();
        }

        void YQLHighlight::Apply(std::string_view query, Colors& colors) {
            Reset(query);

            for (std::size_t i = 0; i < Tokens.size(); ++i) {
                const auto* token = Tokens.get(i);
                const auto color = ColorOf(token);

                const std::ptrdiff_t start = token->getStartIndex();
                const std::ptrdiff_t stop = token->getStopIndex() + 1;

                std::fill(std::next(std::begin(colors), start),
                          std::next(std::begin(colors), stop), color);
            }
        }

        void YQLHighlight::Reset(std::string_view query) {
            Chars.load(query.data(), query.length());
            Lexer.reset();
            Tokens.setTokenSource(&Lexer);

            Tokens.fill();
        }

        YQLHighlight::Color YQLHighlight::ColorOf(const antlr4::Token* token) {
            if (IsString(token)) {
                return Coloring.string;
            }
            if (IsFunctionIdentifier(token)) {
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
            return Coloring.unknown;
        }

        bool YQLHighlight::IsKeyword(const antlr4::Token* token) const {
            return IsYQLKeyword(token->getType());
        }

        bool YQLHighlight::IsOperation(const antlr4::Token* token) const {
            switch (token->getType()) {
                case TOKEN(EQUALS):
                case TOKEN(EQUALS2):
                case TOKEN(NOT_EQUALS):
                case TOKEN(NOT_EQUALS2):
                case TOKEN(LESS):
                case TOKEN(LESS_OR_EQ):
                case TOKEN(GREATER):
                case TOKEN(GREATER_OR_EQ):
                case TOKEN(SHIFT_LEFT):
                case TOKEN(ROT_LEFT):
                case TOKEN(AMPERSAND):
                case TOKEN(PIPE):
                case TOKEN(DOUBLE_PIPE):
                case TOKEN(STRUCT_OPEN):
                case TOKEN(STRUCT_CLOSE):
                case TOKEN(PLUS):
                case TOKEN(MINUS):
                case TOKEN(TILDA):
                case TOKEN(ASTERISK):
                case TOKEN(SLASH):
                case TOKEN(BACKSLASH):
                case TOKEN(PERCENT):
                case TOKEN(SEMICOLON):
                case TOKEN(DOT):
                case TOKEN(COMMA):
                case TOKEN(LPAREN):
                case TOKEN(RPAREN):
                case TOKEN(QUESTION):
                case TOKEN(COLON):
                case TOKEN(AT):
                case TOKEN(DOUBLE_AT):
                case TOKEN(DOLLAR):
                case TOKEN(QUOTE_DOUBLE):
                case TOKEN(QUOTE_SINGLE):
                case TOKEN(BACKTICK):
                case TOKEN(LBRACE_CURLY):
                case TOKEN(RBRACE_CURLY):
                case TOKEN(CARET):
                case TOKEN(NAMESPACE):
                case TOKEN(ARROW):
                case TOKEN(RBRACE_SQUARE):
                case TOKEN(LBRACE_SQUARE):
                    return true;
                default:
                    return false;
            }
        }

        bool YQLHighlight::IsFunctionIdentifier(const antlr4::Token* token) {
            if (token->getType() != TOKEN(ID_PLAIN)) {
                return false;
            }
            const auto index = token->getTokenIndex();
            return std::regex_search(token->getText(), BuiltinFunctionRegex) ||
                   (2 <= index && Tokens.get(index - 1)->getType() == TOKEN(NAMESPACE) && Tokens.get(index - 2)->getType() == TOKEN(ID_PLAIN)) ||
                   (index < Tokens.size() - 1 && Tokens.get(index + 1)->getType() == TOKEN(NAMESPACE));
        }

        bool YQLHighlight::IsTypeIdentifier(const antlr4::Token* token) const {
            return token->getType() == TOKEN(ID_PLAIN) && std::regex_search(token->getText(), TypeRegex);
        }

        bool YQLHighlight::IsVariableIdentifier(const antlr4::Token* token) const {
            return token->getType() == TOKEN(ID_PLAIN);
        }

        bool YQLHighlight::IsQuotedIdentifier(const antlr4::Token* token) const {
            return token->getType() == TOKEN(ID_QUOTED);
        }

        bool YQLHighlight::IsString(const antlr4::Token* token) const {
            return token->getType() == TOKEN(STRING_VALUE);
        }

        bool YQLHighlight::IsNumber(const antlr4::Token* token) const {
            switch (token->getType()) {
                case TOKEN(DIGITS):
                case TOKEN(INTEGER_VALUE):
                case TOKEN(REAL):
                case TOKEN(BLOB):
                    return true;
                default:
                    return false;
            }
        }

    } // namespace NConsoleClient
} // namespace NYdb
