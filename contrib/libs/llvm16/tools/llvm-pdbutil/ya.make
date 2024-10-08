# Generated by devtools/yamaker.

PROGRAM()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/AsmParser
    contrib/libs/llvm16/lib/BinaryFormat
    contrib/libs/llvm16/lib/Bitcode/Reader
    contrib/libs/llvm16/lib/Bitstream/Reader
    contrib/libs/llvm16/lib/DebugInfo/CodeView
    contrib/libs/llvm16/lib/DebugInfo/MSF
    contrib/libs/llvm16/lib/DebugInfo/PDB
    contrib/libs/llvm16/lib/Demangle
    contrib/libs/llvm16/lib/IR
    contrib/libs/llvm16/lib/IRReader
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/MC/MCParser
    contrib/libs/llvm16/lib/Object
    contrib/libs/llvm16/lib/ObjectYAML
    contrib/libs/llvm16/lib/Remarks
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/TargetParser
    contrib/libs/llvm16/lib/TextAPI
)

ADDINCL(
    contrib/libs/llvm16/tools/llvm-pdbutil
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    BytesOutputStyle.cpp
    DumpOutputStyle.cpp
    ExplainOutputStyle.cpp
    MinimalSymbolDumper.cpp
    MinimalTypeDumper.cpp
    PdbYaml.cpp
    PrettyBuiltinDumper.cpp
    PrettyClassDefinitionDumper.cpp
    PrettyClassLayoutGraphicalDumper.cpp
    PrettyCompilandDumper.cpp
    PrettyEnumDumper.cpp
    PrettyExternalSymbolDumper.cpp
    PrettyFunctionDumper.cpp
    PrettyTypeDumper.cpp
    PrettyTypedefDumper.cpp
    PrettyVariableDumper.cpp
    StreamUtil.cpp
    TypeReferenceTracker.cpp
    YAMLOutputStyle.cpp
    llvm-pdbutil.cpp
)

END()
