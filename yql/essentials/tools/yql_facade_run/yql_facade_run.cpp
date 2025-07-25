#include "yql_facade_run.h"

#include <yql/essentials/providers/pg/provider/yql_pg_provider.h>
#include <yql/essentials/providers/common/provider/yql_provider_names.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>
#include <yql/essentials/providers/common/udf_resolve/yql_outproc_udf_resolver.h>
#include <yql/essentials/providers/common/udf_resolve/yql_simple_udf_resolver.h>
#include <yql/essentials/providers/common/udf_resolve/yql_udf_resolver_with_index.h>
#include <yql/essentials/providers/common/udf_resolve/yql_udf_resolver_logger.h>
#include <yql/essentials/core/yql_user_data_storage.h>
#include <yql/essentials/core/yql_udf_resolver.h>
#include <yql/essentials/core/yql_udf_index.h>
#include <yql/essentials/core/yql_udf_index_package_set.h>
#include <yql/essentials/core/yql_library_compiler.h>
#include <yql/essentials/core/yql_type_annotation.h>
#include <yql/essentials/core/pg_ext/yql_pg_ext.h>
#include <yql/essentials/core/services/mounts/yql_mounts.h>
#include <yql/essentials/core/services/yql_out_transformers.h>
#include <yql/essentials/core/file_storage/file_storage.h>
#include <yql/essentials/core/file_storage/proto/file_storage.pb.h>
#include <yql/essentials/core/peephole_opt/yql_opt_peephole_physical.h>
#include <yql/essentials/core/facade/yql_facade.h>
#include <yql/essentials/core/url_lister/url_lister_manager.h>
#include <yql/essentials/core/url_preprocessing/url_preprocessing.h>
#include <yql/essentials/core/qplayer/storage/file/yql_qstorage_file.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/minikql/mkql_function_registry.h>
#include <yql/essentials/ast/yql_expr.h>
#include <yql/essentials/parser/pg_wrapper/interface/parser.h>
#include <yql/essentials/parser/pg_catalog/catalog.h>
#include <yql/essentials/public/udf/udf_version.h>
#include <yql/essentials/public/udf/udf_registrator.h>
#include <yql/essentials/public/udf/udf_validate.h>
#include <yql/essentials/public/result_format/yql_result_format_response.h>
#include <yql/essentials/public/result_format/yql_result_format_type.h>
#include <yql/essentials/public/result_format/yql_result_format_data.h>
#include <yql/essentials/utils/failure_injector/failure_injector.h>
#include <yql/essentials/utils/backtrace/backtrace.h>
#include <yql/essentials/utils/mem_limit.h>
#include <yql/essentials/protos/yql_mount.pb.h>
#include <yql/essentials/protos/pg_ext.pb.h>
#include <yql/essentials/sql/settings/translation_settings.h>
#include <yql/essentials/sql/v1/complete/check/check_complete.h>
#include <yql/essentials/sql/v1/format/sql_format.h>
#include <yql/essentials/sql/v1/sql.h>
#include <yql/essentials/sql/sql.h>
#include <yql/essentials/sql/v1/lexer/check/check_lexers.h>
#include <yql/essentials/sql/v1/lexer/antlr4/lexer.h>
#include <yql/essentials/sql/v1/lexer/antlr4_ansi/lexer.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4/proto_parser.h>
#include <yql/essentials/sql/v1/proto_parser/antlr4_ansi/proto_parser.h>

#include <library/cpp/resource/resource.h>
#include <library/cpp/yson/node/node_io.h>
#include <library/cpp/yson/writer.h>

#include <google/protobuf/text_format.h>
#include <google/protobuf/arena.h>

#include <util/stream/output.h>
#include <util/stream/file.h>
#include <util/stream/null.h>
#include <util/system/user.h>
#include <util/system/env.h>
#include <util/string/split.h>
#include <util/string/join.h>
#include <util/string/builder.h>
#include <util/string/strip.h>
#include <util/generic/vector.h>
#include <util/generic/ptr.h>
#include <util/generic/yexception.h>
#include <util/datetime/base.h>

namespace {

const ui32 PRETTY_FLAGS = NYql::TAstPrintFlags::PerLine | NYql::TAstPrintFlags::ShortQuote |
        NYql::TAstPrintFlags::AdaptArbitraryContent;

template <typename TMessage>
THolder<TMessage> ParseProtoFromResource(TStringBuf resourceName) {
    if (!NResource::Has(resourceName)) {
        return {};
    }
    auto config = MakeHolder<TMessage>();
    TString configData = NResource::Find(resourceName);

    using ::google::protobuf::TextFormat;
    if (!TextFormat::ParseFromString(configData, config.Get())) {
        throw yexception() << "Bad format of config " << resourceName;
    }
    return config;
}

class TOptPipelineConfigurator : public NYql::IPipelineConfigurator {
public:
    TOptPipelineConfigurator(NYql::TProgramPtr prg, IOutputStream* planStream, IOutputStream* exprStream, bool withTypes)
        : Program_(std::move(prg))
        , PlanStream_(planStream)
        , ExprStream_(exprStream)
        , WithTypes_(withTypes)
    {
    }

    void AfterCreate(NYql::TTransformationPipeline* pipeline) const final {
        Y_UNUSED(pipeline);
    }

    void AfterTypeAnnotation(NYql::TTransformationPipeline* pipeline) const final {
        pipeline->Add(NYql::TExprLogTransformer::Sync("OptimizedExpr", NYql::NLog::EComponent::Core, NYql::NLog::ELevel::TRACE),
            "OptTrace", NYql::TIssuesIds::CORE, "OptTrace");
    }

    void AfterOptimize(NYql::TTransformationPipeline* pipeline) const final {
        if (ExprStream_) {
            pipeline->Add(NYql::TExprOutputTransformer::Sync(Program_->ExprRoot(), ExprStream_, WithTypes_), "AstOutput");
        }
        if (PlanStream_) {
            pipeline->Add(NYql::TPlanOutputTransformer::Sync(PlanStream_, Program_->GetPlanBuilder(), Program_->GetOutputFormat()), "PlanOutput");
        }
    }
private:
    NYql::TProgramPtr Program_;
    IOutputStream* PlanStream_;
    IOutputStream* ExprStream_;
    bool WithTypes_;
};

class TPeepHolePipelineConfigurator : public NYql::IPipelineConfigurator {
public:
    TPeepHolePipelineConfigurator() {
    }

    void AfterCreate(NYql::TTransformationPipeline* pipeline) const final {
        Y_UNUSED(pipeline);
    }

    void AfterTypeAnnotation(NYql::TTransformationPipeline* pipeline) const final {
        pipeline->Add(NYql::TExprLogTransformer::Sync("OptimizedExpr", NYql::NLog::EComponent::Core, NYql::NLog::ELevel::TRACE),
            "OptTrace", NYql::TIssuesIds::CORE, "OptTrace");
    }

    void AfterOptimize(NYql::TTransformationPipeline* pipeline) const final {
        pipeline->Add(NYql::MakePeepholeOptimization(pipeline->GetTypeAnnotationContext()), "PeepHole");
    }
};

} // unnamed


namespace NYql {

TFacadeRunOptions::TFacadeRunOptions() {
}

TFacadeRunOptions::~TFacadeRunOptions() {
}

void TFacadeRunOptions::InitLogger() {
    if (Verbosity != LOG_DEF_PRIORITY || ShowLog) {
        NLog::ELevel level = NLog::ELevelHelpers::FromInt(Verbosity);
        if (ShowLog) {
            level = Max(level, NLog::ELevel::DEBUG);
        }
        NLog::EComponentHelpers::ForEach([level](NLog::EComponent c) {
            NYql::NLog::YqlLogger().SetComponentLevel(c, level);
        });
    }

    if (TraceOptStream) {
        NLog::YqlLogger().SetComponentLevel(NLog::EComponent::Core, NLog::ELevel::TRACE);
        NLog::YqlLogger().SetComponentLevel(NLog::EComponent::CoreEval, NLog::ELevel::TRACE);
        NLog::YqlLogger().SetComponentLevel(NLog::EComponent::CorePeepHole, NLog::ELevel::TRACE);
    }
}

void TFacadeRunOptions::PrintInfo(const TString& msg) {
    if (!NoDebug && Verbosity >= TLOG_INFO) {
        Cerr << msg << Endl;
    }
}

void TFacadeRunOptions::ParseProtoConfig(const TString& cfgFile, google::protobuf::Message* config) {
    TString configData = TFileInput(cfgFile).ReadAll();

    using ::google::protobuf::TextFormat;
    if (!TextFormat::ParseFromString(configData, config)) {
        throw yexception() << "Bad format of config file " << cfgFile;
    }
}

void TFacadeRunOptions::Parse(int argc, const char *argv[]) {
    User = GetUsername();

    if (EnableCredentials) {
        Token = GetEnv("YQL_TOKEN");
        if (!Token) {
            const TString home = GetEnv("HOME");
            auto tokenPath = TFsPath(home) / ".yql" / "token";
            if (tokenPath.Exists()) {
                Token = StripStringRight(TFileInput(tokenPath).ReadAll());
            }
        }
    }

    NLastGetopt::TOpts opts = NLastGetopt::TOpts::Default();

    opts.AddHelpOption();
    opts.AddLongOption('p', "program", "Program file (use - to read from stdin)").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            ProgramFile = file;
            if (ProgramFile == "-") {
                ProgramFile = "-stdin-";
                ProgramText = Cin.ReadAll();
            } else {
                ProgramText = TFileInput(ProgramFile).ReadAll();
            }
        });
    opts.AddLongOption('s', "sql", "Program is SQL query").NoArgument().StoreValue(&ProgramType, EProgramType::Sql);
    if (PgSupport) {
        opts.AddLongOption("pg", "Program has PG syntax").NoArgument().StoreValue(&ProgramType, EProgramType::Pg);
        opts.AddLongOption("pg-ext", "Pg extensions config file").Optional().RequiredArgument("FILE")
            .Handler1T<TString>([this](const TString& file) {
                PgExtConfig = TFacadeRunOptions::ParseProtoConfig<NProto::TPgExtensions>(file);
            });
    }
    opts.AddLongOption('f', "file", "Additional files").RequiredArgument("name@path")
        .KVHandler([this](TString name, TString path) {
            if (name.empty() || path.empty()) {
                throw yexception() << "Incorrect file mapping, expected form name@path, e.g. MyFile@file.txt";
            }

            auto& entry = DataTable[NYql::TUserDataKey::File(NYql::GetDefaultFilePrefix() + name)];
            entry.Type = NYql::EUserDataType::PATH;
            entry.Data = path;
        }, '@');

    opts.AddLongOption('U', "url", "Additional urls").RequiredArgument("name@path")
        .KVHandler([this](TString name, TString url) {
            if (name.empty() || url.empty()) {
                throw yexception() << "url mapping, expected form name@url, e.g. MyUrl@http://example.com/file";
            }

            auto& entry = DataTable[NYql::TUserDataKey::File(NYql::GetDefaultFilePrefix() + name)];
            entry.Type = NYql::EUserDataType::URL;
            entry.Data = url;
        }, '@');

    opts.AddLongOption('m', "mounts", "Mount points config file.").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            MountConfig = TFacadeRunOptions::ParseProtoConfig<NYqlMountConfig::TMountConfig>(file);
        });
    opts.AddLongOption("params-file", "Query parameters values in YSON format").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            Params = TFileInput(file).ReadAll();
        });
    opts.AddLongOption("yson-attrs", "Provide operation yson attribues").Optional().RequiredArgument("VALUE").StoreResult(&YsonAttrs);
    opts.AddLongOption('G', "gateways", TStringBuilder() << "Used gateways, available: " << JoinSeq(",", SupportedGateways_)).DefaultValue(JoinSeq(",", GatewayTypes))
        .Handler1T<TString>([this](const TString& gateways) {
            GatewayTypes.clear();
            ::StringSplitter(gateways).Split(',').Consume([&](const TStringBuf& val) {
                if (!SupportedGateways_.contains(val)) {
                    throw yexception() << "Unsupported gateway \"" << val << '"';
                }
                GatewayTypes.emplace(val);
            });
        });
    opts.AddLongOption("gateways-cfg", "Gateways configuration file").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            GatewaysConfig = TFacadeRunOptions::ParseProtoConfig<TGatewaysConfig>(file);
        });
    opts.AddLongOption("fs-cfg", "Fs configuration file").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            FsConfig = MakeHolder<TFileStorageConfig>();
            LoadFsConfigFromFile(file, *FsConfig);
        });
    opts.AddLongOption('u', "udf", "Load shared library with UDF by given path").RequiredArgument("PATH").AppendTo(&UdfsPaths);
    opts.AddLongOption("udfs-dir", "Load all shared libraries with UDFs found in given directory").RequiredArgument("DIR")
        .Handler1T<TString>([this](const TString& dir) {
            NKikimr::NMiniKQL::FindUdfsInDir(dir, &UdfsPaths);
        });
    opts.AddLongOption("udf-resolver", "Path to udf-resolver").Optional().RequiredArgument("PATH").StoreResult(&UdfResolverPath);
    opts.AddLongOption("udf-resolver-log", "Path to udf resolver log").Optional().RequiredArgument("PATH").StoreResult(&UdfResolverLog);
    opts.AddLongOption("udf-resolver-filter-syscalls", "Filter syscalls in udf resolver").Optional().NoArgument().SetFlag(&UdfResolverFilterSyscalls);
    opts.AddLongOption("scan-udfs", "Scan specified udfs with external udf-resolver to use static function registry").NoArgument().SetFlag(&ScanUdfs);

    opts.AddLongOption("parse-only", "Parse program and exit").NoArgument().StoreValue(&Mode, ERunMode::Parse);
    opts.AddLongOption("compile-only", "Compile program and exit").NoArgument().StoreValue(&Mode, ERunMode::Compile);
    opts.AddLongOption("validate", "Validate program and exit").NoArgument().StoreValue(&Mode, ERunMode::Validate);
    opts.AddLongOption("lineage", "Calculate program lineage and exit").NoArgument().StoreValue(&Mode, ERunMode::Lineage);
    opts.AddLongOption('O',"optimize", "Optimize program and exit").NoArgument().StoreValue(&Mode, ERunMode::Optimize);
    opts.AddLongOption('D', "discover", "Discover tables in the program and exit").NoArgument().StoreValue(&Mode, ERunMode::Discover);
    opts.AddLongOption("peephole", "Perform peephole program optimization and exit").NoArgument().StoreValue(&Mode, ERunMode::Peephole);
    opts.AddLongOption('R',"run", "Run program (use by default)").NoArgument().StoreValue(&Mode, ERunMode::Run);

    opts.AddLongOption('L', "show-log", "Show transformation log").Optional().NoArgument().SetFlag(&ShowLog);
    opts.AddLongOption('v', "verbosity", "Log verbosity level").Optional().RequiredArgument("LEVEL").StoreResult(&Verbosity);
    opts.AddLongOption("print-ast", "Print AST after loading").NoArgument().SetFlag(&PrintAst);
    opts.AddLongOption("print-expr", "Print rebuild AST before execution").NoArgument()
        .Handler0([this]() {
            if (!ExprStream) {
                ExprStream = &Cout;
            }
        });
    opts.AddLongOption("with-types", "Print types annotation").NoArgument().SetFlag(&WithTypes);
    opts.AddLongOption("trace-opt", "Print AST in the begin of each transformation").NoArgument()
        .Handler0([this]() {
            TraceOptStream = &Cerr;
        });
    opts.AddLongOption("expr-file", "Print AST to that file instead of stdout").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            ExprStreamHolder_ = MakeHolder<TFixedBufferFileOutput>(file);
            ExprStream = ExprStreamHolder_.Get();
        });
    opts.AddLongOption("print-result", "Print program execution result to stdout").NoArgument()
        .Handler0([this]() {
            if (!ResultStream) {
                ResultStream = &Cout;
            }
        });
    opts.AddLongOption("format", "Results format")
        .Optional()
        .RequiredArgument("STR")
        .Choices(THashSet<TString>{"text", "binary", "pretty"})
        .Handler1T<TString>([this](const TString& val) {
            if (val == "text") {
                ResultsFormat = NYson::EYsonFormat::Text;
            } else if (val == "binary") {
                ResultsFormat = NYson::EYsonFormat::Binary;
            } else if (val == "pretty") {
                ResultsFormat = NYson::EYsonFormat::Pretty;
            } else {
                throw yexception() << "Unknown result format " << val;
            }
        });

    opts.AddLongOption("result-file", "Print program execution result to file").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            ResultStreamHolder_ = MakeHolder<TFixedBufferFileOutput>(file);
            ResultStream = ResultStreamHolder_.Get();
        });
    opts.AddLongOption('P',"trace-plan", "Print plan before execution").NoArgument()
        .Handler0([this]() {
            if (!PlanStream) {
                PlanStream = &Cerr;
            }
        });
    opts.AddLongOption("plan-file", "Print program plan to file").Optional().RequiredArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            PlanStreamHolder_ = MakeHolder<TFixedBufferFileOutput>(file);
            PlanStream = PlanStreamHolder_.Get();
        });
    opts.AddLongOption("err-file", "Print validate/optimize/runtime errors to file")
        .Handler1T<TString>([this](const TString& file) {
            ErrStreamHolder_ = MakeHolder<TFixedBufferFileOutput>(file);
            ErrStream = ErrStreamHolder_.Get();
        });
    opts.AddLongOption("full-expr", "Avoid buffering of expr/plan").NoArgument().SetFlag(&FullExpr);
    opts.AddLongOption("mem-limit", "Set memory limit in megabytes")
        .Handler1T<ui32>(0, SetAddressSpaceLimit);

    opts.AddLongOption("validate-mode", "Validate udf mode, available values: " + NUdf::ValidateModeAvailables())
        .DefaultValue(NUdf::ValidateModeAsStr(NUdf::EValidateMode::Greedy))
        .Handler1T<TString>([this](const TString& mode) {
            ValidateMode = NUdf::ValidateModeByStr(mode);
        });
    opts.AddLongOption("stat", "Print execution statistics").Optional().OptionalArgument("FILE")
        .Handler1T<TString>([this](const TString& file) {
            if (file) {
                StatStreamHolder_ = MakeHolder<TFileOutput>(file);
                StatStream = StatStreamHolder_.Get();
            } else {
                StatStream = &Cerr;
            }
        });
    opts.AddLongOption("full-stat", "Output full execution statistics").Optional().NoArgument().SetFlag(&FullStatistics);

    opts.AddLongOption("sql-flags", "SQL translator pragma flags").SplitHandler(&SqlFlags, ',');
    opts.AddLongOption("syntax-version", "SQL syntax version").StoreResult(&SyntaxVersion).DefaultValue(1);
    opts.AddLongOption("ansi-lexer", "Use ansi lexer").NoArgument().SetFlag(&AnsiLexer);
    opts.AddLongOption("assume-ydb-on-slash", "Assume YDB provider if cluster name starts with '/'").NoArgument().SetFlag(&AssumeYdbOnClusterWithSlash);

    opts.AddLongOption("with-final-issues", "Include some final messages (like statistic) in issues").NoArgument().SetFlag(&WithFinalIssues);
    if (FailureInjectionSupport) {
        opts.AddLongOption("failure-inject", "Activate failure injection")
            .Optional()
            .RequiredArgument("INJECTION_NAME=FAIL_COUNT or INJECTION_NAME=SKIP_COUNT/FAIL_COUNT")
            .KVHandler([](TString name, TString value) {
                TFailureInjector::Activate();
                TStringBuf fail = value;
                TStringBuf skip;
                if (TStringBuf(value).TrySplit('/', skip, fail)) {
                    TFailureInjector::Set(name, FromString<ui32>(skip), FromString<ui32>(fail));
                } else {
                    TFailureInjector::Set(name, 0, FromString<ui32>(fail));
                }
            });
    }
    if (EnableCredentials) {
        opts.AddLongOption("token", "YQL token")
            .Optional()
            .RequiredArgument("VALUE")
            .StoreResult(&Token);
        opts.AddLongOption("custom-tokens", "Custom tokens")
            .Optional()
            .RequiredArgument("NAME=VALUE or NAME=@PATH")
            .KVHandler([this](TString key, TString value) {
                if (value.StartsWith('@')) {
                    value = StripStringRight(TFileInput(value.substr(1)).ReadAll());
                }
                Credentials->AddCredential(key, TCredential("custom", "", value));
            });
    }
    if (EnableQPlayer) {
        opts.AddLongOption("qstorage-dir", "Directory for QStorage").RequiredArgument("DIR")
            .Handler1T<TString>([this](const TString& dir) {
                QPlayerStorage_ = MakeFileQStorage(dir);
            });
        opts.AddLongOption("op-id", "QStorage operation id").StoreResult(&OperationId).DefaultValue("dummy_op");
        opts.AddLongOption("capture", "Write query metadata to QStorage").NoArgument()
            .Handler0([this]() {
                if (EQPlayerMode::Replay == QPlayerMode) {
                    throw yexception() << "replay and capture options can't be used simultaneously";
                }
                QPlayerMode = EQPlayerMode::Capture;
            });
        opts.AddLongOption("replay", "Read query metadata from QStorage").NoArgument()
            .Handler0([this]() {
                if (EQPlayerMode::Capture == QPlayerMode) {
                    throw yexception() << "replay and capture options can't be used simultaneously";
                }
                QPlayerMode = EQPlayerMode::Replay;
            });
        opts.AddLongOption("gateways-patch", "QPlayer patch for gateways conf").Optional().RequiredArgument("FILE")
            .Handler1T<TString>([this](const TString& file) {
                GatewaysPatch = TFileInput(file).ReadAll();
            });
    }

    if (CustomTests) {
        opts.AddLongOption("test-antlr4", "Check antlr4 parser").NoArgument().SetFlag(&TestAntlr4);
        opts.AddLongOption("test-format", "Compare formatted query's AST with the original query's AST (only syntaxVersion=1 is supported)").NoArgument().SetFlag(&TestSqlFormat);
        opts.AddLongOption("test-lexers", "Compare lexers").NoArgument().SetFlag(&TestLexers);
        opts.AddLongOption("test-complete", "check completion engine").NoArgument().SetFlag(&TestComplete);
        opts.AddLongOption("validate-result-format", "Check that result-format can parse Result").NoArgument().SetFlag(&ValidateResultFormat);
    }

    opts.AddLongOption("langver", "Set current language version").Optional().RequiredArgument("VER")
        .Handler1T<TString>([this](const TString& str) {
            if (str == "unknown") {
                LangVer = UnknownLangVersion;
            } else if (!ParseLangVersion(str, LangVer)) {
                throw yexception() << "Failed to parse language version: " << str;
            }
        });

    opts.AddLongOption("max-langver", "Set maximum language version").Optional().RequiredArgument("VER")
        .Handler1T<TString>([this](const TString& str) {
            if (!ParseLangVersion(str, MaxLangVer)) {
                throw yexception() << "Failed to parse language version: " << str;
            }
        });

    opts.SetFreeArgsMax(0);

    for (auto& ext: OptExtenders_) {
        ext(opts);
    }

    auto res = NLastGetopt::TOptsParseResult(&opts, argc, argv);

    if (QPlayerMode != EQPlayerMode::None) {
        if (!QPlayerStorage_) {
            QPlayerStorage_ = MakeFileQStorage(".");
        }
        if (EQPlayerMode::Replay == QPlayerMode) {
            try {
                QPlayerContext = TQContext(QPlayerStorage_->MakeReader(OperationId, {}));
            } catch (...) {
                throw yexception() << "QPlayer replay is probably broken. Exception: " << CurrentExceptionMessage();
            }
            ProgramFile = "-replay-";
            ProgramText = "";
        } else if (EQPlayerMode::Capture == QPlayerMode) {
            QPlayerContext = TQContext(QPlayerStorage_->MakeWriter(OperationId, {}));
        }
    }
    if (EQPlayerMode::Replay != QPlayerMode && !ProgramText) {
        throw yexception() << "Either program or replay option should be specified";
    }
    if (GatewaysPatch && EQPlayerMode::Replay != QPlayerMode) {
        throw yexception() << "gateways-patch can only be used with replay option";
    }

    if (Mode >= ERunMode::Validate && GatewayTypes.empty()) {
        throw yexception() << "At least one gateway from the list " << JoinSeq(",", SupportedGateways_).Quote() << " must be specified";
    }

    if (!GatewaysConfig) {
        GatewaysConfig = ParseProtoFromResource<TGatewaysConfig>("gateways.conf");
    }

    if (GatewaysConfig && GatewaysConfig->HasSqlCore()) {
        SqlFlags.insert(GatewaysConfig->GetSqlCore().GetTranslationFlags().begin(), GatewaysConfig->GetSqlCore().GetTranslationFlags().end());
    }
    UpdateSqlFlagsFromQContext(QPlayerContext, SqlFlags);

    if (!FsConfig) {
        FsConfig = MakeHolder<TFileStorageConfig>();
        if (NResource::Has("fs.conf")) {
            LoadFsConfigFromResource("fs.conf", *FsConfig);
        }
    }

    if (EnableCredentials && Token) {
        for (auto name: SupportedGateways_) {
            Credentials->AddCredential(TStringBuilder() << "default_" << name, TCredential(name, "", Token));
        }
    }

    for (auto& handle: OptHandlers_) {
        handle(res);
    }
}

TFacadeRunner::TFacadeRunner(TString name)
    : Name_(std::move(name))
    , YqlLogger_(std::make_unique<NYql::NLog::YqlLoggerScope>(&Cerr))
{
}

TFacadeRunner::~TFacadeRunner() {
}

TIntrusivePtr<NKikimr::NMiniKQL::IFunctionRegistry> TFacadeRunner::GetFuncRegistry() {
    return FuncRegistry_;
}

int TFacadeRunner::Main(int argc, const char *argv[]) {
    NYql::NBacktrace::RegisterKikimrFatalActions();
    NYql::NBacktrace::EnableKikimrSymbolize();
    EnableKikimrBacktraceFormat();

    try {
        return DoMain(argc, argv);
    }
    catch (...) {
        Cerr << CurrentExceptionMessage() << Endl;
        return 1;
    }
}

int TFacadeRunner::DoMain(int argc, const char *argv[]) {
    Y_UNUSED(NUdf::GetStaticSymbols());

    RunOptions_.Parse(argc, argv);

    if (!RunOptions_.NoDebug) {
        Cerr << Name_ << " ABI version: " << NKikimr::NUdf::CurrentAbiVersionStr() << Endl;
    }

    RunOptions_.InitLogger();

    if (RunOptions_.PgSupport) {
        ClusterMapping_["pg_catalog"] = PgProviderName;
        ClusterMapping_["information_schema"] = PgProviderName;

        NPg::SetSqlLanguageParser(NSQLTranslationPG::CreateSqlLanguageParser());
        NPg::LoadSystemFunctions(*NSQLTranslationPG::CreateSystemFunctionsParser());
        if (RunOptions_.PgExtConfig) {
            TVector<NPg::TExtensionDesc> extensions;
            PgExtensionsFromProto(*RunOptions_.PgExtConfig, extensions);
            NPg::RegisterExtensions(extensions, RunOptions_.QPlayerContext.CanRead(),
                *NSQLTranslationPG::CreateExtensionSqlParser(),
                NKikimr::NMiniKQL::CreateExtensionLoader().get());
        }

        NPg::GetSqlLanguageParser()->Freeze();
    }

    auto funcRegistry = NKikimr::NMiniKQL::CreateFunctionRegistry(&NYql::NBacktrace::KikimrBackTrace,
        NKikimr::NMiniKQL::CreateBuiltinRegistry(), true, RunOptions_.UdfsPaths)->Clone();
    NKikimr::NMiniKQL::FillStaticModules(*funcRegistry);
    FuncRegistry_ = funcRegistry;

    NSQLTranslationV1::TLexers lexers;
    lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
    lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
    NSQLTranslationV1::TParsers parsers;
    parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
    parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();

    NSQLTranslation::TTranslators translators(
        nullptr,
        NSQLTranslationV1::MakeTranslator(lexers, parsers),
        NSQLTranslationPG::MakeTranslator()
    );

    TExprContext ctx;
    if (RunOptions_.PgSupport) {
        ctx.NextUniqueId = NPg::GetSqlLanguageParser()->GetContext().NextUniqueId;
    }
    IModuleResolver::TPtr moduleResolver;
    TModuleResolver::TModuleChecker moduleChecker;
    if (RunOptions_.TestLexers || RunOptions_.TestComplete) {
        moduleChecker = [
            lexers, parsers,
            testLexers = RunOptions_.TestLexers,
            testComplete = RunOptions_.TestComplete,
            clusters = ClusterMapping_](const TString& query, const TString& fileName, TExprContext& ctx) {

            if (testLexers) {
                TIssues issues;
                if (!NSQLTranslationV1::CheckLexers(TPosition(0, 0, fileName), query, issues)) {
                    auto issue = TIssue(TPosition(0, 0, fileName), "Lexers mismatched");
                    for (const auto& i : issues) {
                        issue.AddSubIssue(MakeIntrusive<TIssue>(i));
                    }

                    ctx.AddError(issue);
                    return false;
                }
            }

            if (testComplete) {
                google::protobuf::Arena arena;

                NSQLTranslation::TTranslationSettings settings;
                settings.Arena = &arena;
                settings.ClusterMapping = clusters;
                settings.SyntaxVersion = 1;

                auto ast = NSQLTranslationV1::SqlToYql(lexers, parsers, query, settings);
                if (!ast.IsOk()) {
                    return true;
                }

                TIssues issues;
                if (!NSQLComplete::CheckComplete(query, *ast.Root, issues)) {
                    auto issue = TIssue(TPosition(0, 0, fileName), "Completion failed");
                    for (const auto& i : issues) {
                        issue.AddSubIssue(MakeIntrusive<TIssue>(i));
                    }

                    ctx.AddError(issue);
                    return false;
                }
            }

            return true;
        };
    }

    if (RunOptions_.MountConfig) {
        TModulesTable modules;
        FillUserDataTableFromFileSystem(*RunOptions_.MountConfig, RunOptions_.DataTable);

        if (!CompileLibraries(translators, RunOptions_.DataTable, ctx, modules, RunOptions_.OptimizeLibs && RunOptions_.Mode >= ERunMode::Validate)) {
            *RunOptions_.ErrStream << "Errors on compile libraries:" << Endl;
            ctx.IssueManager.GetIssues().PrintTo(*RunOptions_.ErrStream);
            return -1;
        }

        moduleResolver = std::make_shared<TModuleResolver>(translators, std::move(modules), ctx.NextUniqueId,
            ClusterMapping_, RunOptions_.SqlFlags, RunOptions_.Mode >= ERunMode::Validate, THolder<TExprContext>(), moduleChecker);
    } else {
        if (!GetYqlDefaultModuleResolver(ctx, moduleResolver, ClusterMapping_,
            RunOptions_.OptimizeLibs && RunOptions_.Mode >= ERunMode::Validate, moduleChecker)) {
            *RunOptions_.ErrStream << "Errors loading default YQL libraries:" << Endl;
            ctx.IssueManager.GetIssues().PrintTo(*RunOptions_.ErrStream);
            return -1;
        }
    }

    TExprContext::TFreezeGuard freezeGuard(ctx);

    if (RunOptions_.Mode >= ERunMode::Validate) {
        std::vector<NFS::IDownloaderPtr> downloaders;
        for (auto& factory: FsDownloadFactories_) {
            if (auto download = factory()) {
                downloaders.push_back(std::move(download));
            }
        }

        FileStorage_ = WithAsync(CreateFileStorage(*RunOptions_.FsConfig, downloaders));
    }

    IUdfResolver::TPtr udfResolver;
    TUdfIndex::TPtr udfIndex;
    if (FileStorage_ && RunOptions_.ScanUdfs) {
        if (!RunOptions_.UdfResolverPath && EQPlayerMode::Replay != RunOptions_.QPlayerMode) {
            Cerr << "udf-resolver path must be specified when use 'scan-udfs'";
            return -1;
        }
        udfResolver = NCommon::CreateOutProcUdfResolver(FuncRegistry_.Get(), FileStorage_, RunOptions_.UdfResolverPath, {}, {}, RunOptions_.UdfResolverFilterSyscalls, {});

        udfIndex = new TUdfIndex();
        if (EQPlayerMode::Replay != RunOptions_.QPlayerMode) {
            THoldingFileStorage storage(FileStorage_);
            RunOptions_.PrintInfo(TStringBuilder() << TInstant::Now().ToStringLocalUpToSeconds() << " Udf scanning started for " << RunOptions_.UdfsPaths.size() << " udfs ...");
            LoadRichMetadataToUdfIndex(*udfResolver, RunOptions_.UdfsPaths, false, TUdfIndex::EOverrideMode::RaiseError, *udfIndex, storage);
            RunOptions_.PrintInfo(TStringBuilder() << TInstant::Now().ToStringLocalUpToSeconds() << " UdfIndex done.");
        }

        if (RunOptions_.UdfResolverLog) {
            udfResolver = NCommon::CreateUdfResolverDecoratorWithLogger(FuncRegistry_.Get(), udfResolver, RunOptions_.UdfResolverLog, RunOptions_.OperationId);
        }

        udfResolver = NCommon::CreateUdfResolverWithIndex(udfIndex, udfResolver, FileStorage_);
        RunOptions_.PrintInfo(TStringBuilder() << TInstant::Now().ToStringLocalUpToSeconds() << " Udfs scanned");
    } else {
        udfResolver = FileStorage_ && RunOptions_.UdfResolverPath
            ? NCommon::CreateOutProcUdfResolver(FuncRegistry_.Get(), FileStorage_, RunOptions_.UdfResolverPath, {}, {}, RunOptions_.UdfResolverFilterSyscalls, {})
            : NCommon::CreateSimpleUdfResolver(FuncRegistry_.Get(), FileStorage_, true);
        if (RunOptions_.UdfResolverLog) {
            udfResolver = NCommon::CreateUdfResolverDecoratorWithLogger(FuncRegistry_.Get(), udfResolver, RunOptions_.UdfResolverLog, RunOptions_.OperationId);
        }
    }

    TVector<TDataProviderInitializer> dataProvidersInit;
    if (RunOptions_.PgSupport) {
        dataProvidersInit.push_back(GetPgDataProviderInitializer());
    }
    for (auto& factory: ProviderFactories_) {
        if (auto init = factory()) {
            dataProvidersInit.push_back(std::move(init));
        }
    }

    TVector<IUrlListerPtr> urlListers;
    for (auto& factory: UrlListerFactories_) {
        if (auto listener = factory()) {
            urlListers.push_back(std::move(listener));
        }
    }

    TProgramFactory factory(RunOptions_.UseRepeatableRandomAndTimeProviders, FuncRegistry_.Get(), ctx.NextUniqueId, dataProvidersInit, Name_);
    factory.AddUserDataTable(RunOptions_.DataTable);
    factory.SetModules(moduleResolver);
    factory.SetFileStorage(FileStorage_);
    if (RunOptions_.GatewaysConfig && RunOptions_.GatewaysConfig->HasFs()) {
        factory.SetUrlPreprocessing(new NYql::TUrlPreprocessing(*RunOptions_.GatewaysConfig));
    }
    factory.SetUdfIndex(udfIndex, new TUdfIndexPackageSet());
    factory.SetUdfResolver(udfResolver);
    factory.SetGatewaysConfig(RunOptions_.GatewaysConfig.Get());
    factory.SetCredentials(RunOptions_.Credentials);
    factory.EnableRangeComputeFor();
    if (!urlListers.empty()) {
        factory.SetUrlListerManager(MakeUrlListerManager(urlListers));
    }

    int result = DoRun(factory);
    if (result == 0 && EQPlayerMode::Capture == RunOptions_.QPlayerMode) {
        RunOptions_.QPlayerContext.GetWriter()->Commit().GetValueSync();
    }
    return result;
}

int TFacadeRunner::DoRun(TProgramFactory& factory) {

    TProgramPtr program = factory.Create(RunOptions_.ProgramFile, RunOptions_.ProgramText, RunOptions_.OperationId, EHiddenMode::Disable, RunOptions_.QPlayerContext, RunOptions_.GatewaysPatch);
    program->SetLanguageVersion(RunOptions_.LangVer);
    program->SetMaxLanguageVersion(RunOptions_.MaxLangVer);
    if (RunOptions_.Params) {
        program->SetParametersYson(RunOptions_.Params);
    }

    if (RunOptions_.YsonAttrs) {
        program->SetOperationAttrsYson(RunOptions_.YsonAttrs);
    }

    if (RunOptions_.EnableResultPosition) {
        program->EnableResultPosition();
    }

    if (ProgressWriter_) {
        program->SetProgressWriter(ProgressWriter_);
    }
    program->SetUseTableMetaFromGraph(RunOptions_.UseMetaFromGrpah);
    program->SetValidateOptions(RunOptions_.ValidateMode);

    bool fail = false;
    if (RunOptions_.ProgramType != EProgramType::SExpr) {
        RunOptions_.PrintInfo("Parse SQL...");
        google::protobuf::Arena arena;
        NSQLTranslation::TTranslationSettings settings;
        settings.Arena = &arena;
        settings.PgParser = EProgramType::Pg == RunOptions_.ProgramType;
        settings.ClusterMapping = ClusterMapping_;
        settings.Flags = RunOptions_.SqlFlags;
        settings.SyntaxVersion = RunOptions_.SyntaxVersion;
        settings.AnsiLexer = RunOptions_.AnsiLexer;
        settings.TestAntlr4 = RunOptions_.TestAntlr4;
        settings.V0Behavior = NSQLTranslation::EV0Behavior::Report;
        settings.AssumeYdbOnClusterWithSlash = RunOptions_.AssumeYdbOnClusterWithSlash;
        settings.Bindings = RunOptions_.Bindings;
        if (ERunMode::Discover == RunOptions_.Mode) {
            settings.Mode = NSQLTranslation::ESqlMode::DISCOVERY;
        }
        if (!program->ParseSql(settings)) {
            program->PrintErrorsTo(*RunOptions_.ErrStream);
            fail = true;
        }
        if (!fail && RunOptions_.TestSqlFormat && 1 == RunOptions_.SyntaxVersion) {
            TString formattedProgramText;
            NYql::TIssues issues;
            NSQLTranslationV1::TLexers lexers;
            lexers.Antlr4 = NSQLTranslationV1::MakeAntlr4LexerFactory();
            lexers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiLexerFactory();
            NSQLTranslationV1::TParsers parsers;
            parsers.Antlr4 = NSQLTranslationV1::MakeAntlr4ParserFactory();
            parsers.Antlr4Ansi = NSQLTranslationV1::MakeAntlr4AnsiParserFactory();
            auto formatter = NSQLFormat::MakeSqlFormatter(lexers, parsers, settings);
            if (!formatter->Format(RunOptions_.ProgramText, formattedProgramText, issues)) {
                *RunOptions_.ErrStream << "Format failed" << Endl;
                issues.PrintTo(*RunOptions_.ErrStream);
                return -1;
            }

            auto frmProgram = factory.Create("formatted SQL", formattedProgramText);
            if (!frmProgram->ParseSql(settings)) {
                frmProgram->PrintErrorsTo(*RunOptions_.ErrStream);
                return -1;
            }

            TStringStream srcQuery, frmQuery;

            program->AstRoot()->PrettyPrintTo(srcQuery, PRETTY_FLAGS);
            frmProgram->AstRoot()->PrettyPrintTo(frmQuery, PRETTY_FLAGS);
            if (srcQuery.Str() != frmQuery.Str()) {
                *RunOptions_.ErrStream << "source query's AST and formatted query's AST are not same" << Endl;
                return -1;
            }
        }
        if (!fail && RunOptions_.TestLexers && 1 == RunOptions_.SyntaxVersion) {
            TIssues issues;
            if (!NSQLTranslationV1::CheckLexers({}, RunOptions_.ProgramText, issues)) {
                *RunOptions_.ErrStream << "Lexers mismatched" << Endl;
                issues.PrintTo(*RunOptions_.ErrStream);
                return -1;
            }
        }
        if (!fail && RunOptions_.TestComplete && 1 == RunOptions_.SyntaxVersion) {
            TIssues issues;
            if (!NSQLComplete::CheckComplete(
                    RunOptions_.ProgramText,
                    program->ExprRoot(),
                    program->ExprCtx(),
                    issues)) {
                *RunOptions_.ErrStream << "Completion failed" << Endl;
                issues.PrintTo(*RunOptions_.ErrStream);
                return -1;
            }
        }
    } else {
        RunOptions_.PrintInfo("Parse YQL...");
        if (!program->ParseYql()) {
            program->PrintErrorsTo(*RunOptions_.ErrStream);
            fail = true;
        }
    }

    if (RunOptions_.TraceOptStream) {
        if (auto ast = program->GetQueryAst()) {
            *RunOptions_.TraceOptStream << *ast << Endl;
        }
    }
    if (fail) {
        return -1;
    }

    if (RunOptions_.PrintAst) {
        program->AstRoot()->PrettyPrintTo(Cout, PRETTY_FLAGS);
    }

    if (ERunMode::Parse == RunOptions_.Mode) {
        return 0;
    }

    RunOptions_.PrintInfo("Compile program...");
    if (!program->Compile(RunOptions_.User)) {
        program->PrintErrorsTo(*RunOptions_.ErrStream);
        fail = true;
    }

    if (RunOptions_.TraceOptStream) {
        program->Print(RunOptions_.TraceOptStream, nullptr);
    }
    if (fail) {
        return -1;
    }

    if (ERunMode::Compile == RunOptions_.Mode) {
        if (RunOptions_.ExprStream) {
            auto baseAst = ConvertToAst(*program->ExprRoot(), program->ExprCtx(), NYql::TExprAnnotationFlags::None, true);
            baseAst.Root->PrettyPrintTo(*RunOptions_.ExprStream, PRETTY_FLAGS);
        }
        return 0;
    }

    TProgram::TStatus status = DoRunProgram(program);

    if (ERunMode::Peephole == RunOptions_.Mode && RunOptions_.ExprStream && program->ExprRoot()) {
        auto ast = ConvertToAst(*program->ExprRoot(), program->ExprCtx(), RunOptions_.WithTypes ? TExprAnnotationFlags::Types : TExprAnnotationFlags::None, true);
        ui32 prettyFlags = TAstPrintFlags::ShortQuote;
        if (!RunOptions_.WithTypes) {
            prettyFlags |= TAstPrintFlags::PerLine;
        }
        ast.Root->PrettyPrintTo(*RunOptions_.ExprStream, prettyFlags);
    }

    if (RunOptions_.WithFinalIssues) {
        program->FinalizeIssues();
    }
    program->PrintErrorsTo(*RunOptions_.ErrStream);
    if (status == TProgram::TStatus::Error) {
        if (RunOptions_.TraceOptStream) {
            program->Print(RunOptions_.TraceOptStream, nullptr);
        }
        return -1;
    }

    if (!RunOptions_.FullExpr && ERunMode::Peephole != RunOptions_.Mode) {
        program->Print(RunOptions_.ExprStream, RunOptions_.PlanStream, /*cleanPlan*/true);
    }

    program->ConfigureYsonResultFormat(RunOptions_.ResultsFormat);

    if (RunOptions_.ResultStream) {
        RunOptions_.PrintInfo("Getting results...");
        if (ERunMode::Discover == RunOptions_.Mode) {
            if (auto data = program->GetDiscoveredData()) {
                *RunOptions_.ResultStream << *data;
            }
        } else if (ERunMode::Lineage == RunOptions_.Mode) {
            if (auto data = program->GetLineage()) {
                TStringInput in(*data);
                NYson::ReformatYsonStream(&in, RunOptions_.ResultStream, RunOptions_.ResultsFormat);
            }
        } else if (program->HasResults()) {
            if (RunOptions_.ValidateResultFormat) {
                auto str = program->ResultsAsString();
                if (!str.empty()) {
                    auto node = NYT::NodeFromYsonString(str);
                    for (const auto& r : NResult::ParseResponse(node)) {
                        for (const auto& write : r.Writes) {
                            if (write.Type) {
                                NResult::TEmptyTypeVisitor visitor;
                                NResult::ParseType(*write.Type, visitor);
                            }

                            if (write.Type && write.Data) {
                                NResult::TEmptyDataVisitor visitor;
                                NResult::ParseData(*write.Type, *write.Data, visitor);
                            }
                        }
                    }
                }

                RunOptions_.ResultStream->Write(str.data(), str.size());
            } else {
                *RunOptions_.ResultStream << program->ResultsAsString();
            }
        }
    }

    if (RunOptions_.StatStream) {
        if (auto st = program->GetStatistics(!RunOptions_.FullStatistics)) {
            *RunOptions_.StatStream << *st;
        }
    }

    RunOptions_.PrintInfo("");
    RunOptions_.PrintInfo("Done");

    return 0;
}

TProgram::TStatus TFacadeRunner::DoRunProgram(TProgramPtr program) {
    TProgram::TStatus status = TProgram::TStatus::Ok;

    auto defOptConfig = TOptPipelineConfigurator(program, RunOptions_.FullExpr ? RunOptions_.PlanStream : nullptr, RunOptions_.FullExpr ? RunOptions_.ExprStream : nullptr, RunOptions_.WithTypes);
    IPipelineConfigurator* optConfig = OptPipelineConfigurator_ ? OptPipelineConfigurator_  : &defOptConfig;

    if (ERunMode::Peephole == RunOptions_.Mode) {
        RunOptions_.PrintInfo("Peephole...");
        auto defConfig = TPeepHolePipelineConfigurator();
        IPipelineConfigurator* config = PeepholePipelineConfigurator_ ? PeepholePipelineConfigurator_  : &defConfig;
        status = program->OptimizeWithConfig(RunOptions_.User, *config);
    } else if (ERunMode::Run == RunOptions_.Mode) {
        RunOptions_.PrintInfo("Run program...");
        status = program->RunWithConfig(RunOptions_.User, *optConfig);
    } else if (ERunMode::Optimize == RunOptions_.Mode) {
        RunOptions_.PrintInfo("Optimize program...");
        status = program->OptimizeWithConfig(RunOptions_.User, *optConfig);
    } else if (ERunMode::Validate == RunOptions_.Mode) {
        RunOptions_.PrintInfo("Validate program...");
        status = program->Validate(RunOptions_.User, RunOptions_.ExprStream, RunOptions_.WithTypes);
    } else if (ERunMode::Discover == RunOptions_.Mode) {
        RunOptions_.PrintInfo("Discover program...");
        status = program->Discover(RunOptions_.User);
    } else if (ERunMode::Lineage == RunOptions_.Mode) {
        RunOptions_.PrintInfo("Calculating lineage in program...");
        status = program->LineageWithConfig(RunOptions_.User, *optConfig);
    }

    return status;
}

} // NYql
