# Generated by devtools/yamaker.

LIBRARY()

VERSION(16.0.0)

LICENSE(
    Apache-2.0 WITH LLVM-exception AND
    MIT
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/clang16
    contrib/libs/clang16/include
    contrib/libs/clang16/lib/AST
    contrib/libs/clang16/lib/ASTMatchers
    contrib/libs/clang16/lib/Analysis
    contrib/libs/clang16/lib/Basic
    contrib/libs/clang16/lib/Format
    contrib/libs/clang16/lib/Frontend
    contrib/libs/clang16/lib/Lex
    contrib/libs/clang16/lib/Rewrite
    contrib/libs/clang16/lib/Sema
    contrib/libs/clang16/lib/Serialization
    contrib/libs/clang16/lib/StaticAnalyzer/Core
    contrib/libs/clang16/lib/StaticAnalyzer/Frontend
    contrib/libs/clang16/lib/Tooling
    contrib/libs/clang16/lib/Tooling/Core
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/Frontend/OpenMP
    contrib/libs/llvm16/lib/Support
)

ADDINCL(
    contrib/libs/clang16/tools/extra/clang-tidy
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCDIR(contrib/libs/clang16/tools/extra/clang-tidy)

SRCS(
    ClangTidy.cpp
    ClangTidyCheck.cpp
    ClangTidyDiagnosticConsumer.cpp
    ClangTidyModule.cpp
    ClangTidyOptions.cpp
    ClangTidyProfiling.cpp
    ExpandModularHeadersPPCallbacks.cpp
    GlobList.cpp
    NoLintDirectiveHandler.cpp
)

END()
