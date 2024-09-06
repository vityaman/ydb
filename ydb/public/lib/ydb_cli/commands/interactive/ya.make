LIBRARY()

SRCS(
    interactive_cli.cpp
    line_reader.cpp
    yql_highlight.cpp
    yql_suggestion_engine.cpp
    yql_syntax.cpp
)

PEERDIR(
    contrib/restricted/patched/replxx
    contrib/libs/antlr4_cpp_runtime
    ydb/library/yql/parser/proto_ast/gen/v1_antlr4
    ydb/public/lib/ydb_cli/commands/interactive/antlr4-c3
    ydb/public/lib/ydb_cli/common
)

END()

RECURSE_FOR_TESTS(
    ut
)
