#include "raw_requests.h"

#include "raw_batch_request.h"
#include "rpc_parameters_serialization.h"

#include <yt/cpp/mapreduce/common/helpers.h>
#include <yt/cpp/mapreduce/common/retry_lib.h>
#include <yt/cpp/mapreduce/common/wait_proxy.h>

#include <yt/cpp/mapreduce/http/fwd.h>
#include <yt/cpp/mapreduce/http/context.h>
#include <yt/cpp/mapreduce/http/helpers.h>
#include <yt/cpp/mapreduce/http/http_client.h>
#include <yt/cpp/mapreduce/http/retry_request.h>

#include <yt/cpp/mapreduce/interface/config.h>
#include <yt/cpp/mapreduce/interface/client.h>
#include <yt/cpp/mapreduce/interface/fluent.h>
#include <yt/cpp/mapreduce/interface/operation.h>
#include <yt/cpp/mapreduce/interface/serialize.h>
#include <yt/cpp/mapreduce/interface/tvm.h>

#include <yt/cpp/mapreduce/interface/logging/yt_log.h>

#include <yt/cpp/mapreduce/io/helpers.h>

#include <library/cpp/yson/node/node_io.h>

#include <util/generic/guid.h>
#include <util/generic/scope.h>

namespace NYT::NDetail::NRawClient {

////////////////////////////////////////////////////////////////////////////////

TOperationAttributes ParseOperationAttributes(const TNode& node)
{
    const auto& mapNode = node.AsMap();
    TOperationAttributes result;

    if (auto idNode = mapNode.FindPtr("id")) {
        result.Id = GetGuid(idNode->AsString());
    }

    if (auto typeNode = mapNode.FindPtr("type")) {
        result.Type = FromString<EOperationType>(typeNode->AsString());
    } else if (auto operationTypeNode = mapNode.FindPtr("operation_type")) {
        // COMPAT(levysotsky): (YT-24340) "operation_type" is a deprecated synonym for "type".
        // This branch should be removed when all clusters are updated.
        result.Type = FromString<EOperationType>(operationTypeNode->AsString());
    }

    if (auto stateNode = mapNode.FindPtr("state")) {
        result.State = stateNode->AsString();
        // We don't use FromString here, because OS_IN_PROGRESS unites many states: "initializing", "running", etc.
        if (*result.State == "completed") {
            result.BriefState = EOperationBriefState::Completed;
        } else if (*result.State == "aborted") {
            result.BriefState = EOperationBriefState::Aborted;
        } else if (*result.State == "failed") {
            result.BriefState = EOperationBriefState::Failed;
        } else {
            result.BriefState = EOperationBriefState::InProgress;
        }
    }
    if (auto authenticatedUserNode = mapNode.FindPtr("authenticated_user")) {
        result.AuthenticatedUser = authenticatedUserNode->AsString();
    }
    if (auto startTimeNode = mapNode.FindPtr("start_time")) {
        result.StartTime = TInstant::ParseIso8601(startTimeNode->AsString());
    }
    if (auto finishTimeNode = mapNode.FindPtr("finish_time")) {
        result.FinishTime = TInstant::ParseIso8601(finishTimeNode->AsString());
    }
    auto briefProgressNode = mapNode.FindPtr("brief_progress");
    if (briefProgressNode && briefProgressNode->HasKey("jobs")) {
        result.BriefProgress.ConstructInPlace();
        static auto load = [] (const TNode& item) {
            // Backward compatibility with old YT versions
            return item.IsInt64() ? item.AsInt64() : item["total"].AsInt64();
        };
        const auto& jobs = (*briefProgressNode)["jobs"];
        result.BriefProgress->Aborted = load(jobs["aborted"]);
        result.BriefProgress->Completed = load(jobs["completed"]);
        result.BriefProgress->Running = jobs["running"].AsInt64();
        result.BriefProgress->Total = jobs["total"].AsInt64();
        result.BriefProgress->Failed = jobs["failed"].AsInt64();
        result.BriefProgress->Lost = jobs["lost"].AsInt64();
        result.BriefProgress->Pending = jobs["pending"].AsInt64();
    }
    if (auto briefSpecNode = mapNode.FindPtr("brief_spec")) {
        result.BriefSpec = *briefSpecNode;
    }
    if (auto specNode = mapNode.FindPtr("spec")) {
        result.Spec = *specNode;
    }
    if (auto fullSpecNode = mapNode.FindPtr("full_spec")) {
        result.FullSpec = *fullSpecNode;
    }
    if (auto unrecognizedSpecNode = mapNode.FindPtr("unrecognized_spec")) {
        result.UnrecognizedSpec = *unrecognizedSpecNode;
    }
    if (auto suspendedNode = mapNode.FindPtr("suspended")) {
        result.Suspended = suspendedNode->AsBool();
    }
    if (auto resultNode = mapNode.FindPtr("result")) {
        result.Result.ConstructInPlace();
        auto error = TYtError((*resultNode)["error"]);
        if (error.GetCode() != 0) {
            result.Result->Error = std::move(error);
        }
    }
    if (auto progressNode = mapNode.FindPtr("progress")) {
        const auto& progressMap = progressNode->AsMap();
        TMaybe<TInstant> buildTime;
        if (auto buildTimeNode = progressMap.FindPtr("build_time")) {
            buildTime = TInstant::ParseIso8601(buildTimeNode->AsString());
        }
        TJobStatistics jobStatistics;
        if (auto jobStatisticsNode = progressMap.FindPtr("job_statistics")) {
            jobStatistics = TJobStatistics(*jobStatisticsNode);
        }
        TJobCounters jobCounters;
        if (auto jobCountersNode = progressMap.FindPtr("total_job_counter")) {
            jobCounters = TJobCounters(*jobCountersNode);
        }
        result.Progress = TOperationProgress{
            .JobStatistics = std::move(jobStatistics),
            .JobCounters = std::move(jobCounters),
            .BuildTime = buildTime,
        };
    }
    if (auto eventsNode = mapNode.FindPtr("events")) {
        result.Events.ConstructInPlace().reserve(eventsNode->Size());
        for (const auto& eventNode : eventsNode->AsList()) {
            result.Events->push_back(TOperationEvent{
                eventNode["state"].AsString(),
                TInstant::ParseIso8601(eventNode["time"].AsString()),
            });
        }
    }
    if (auto alertsNode = mapNode.FindPtr("alerts")) {
        result.Alerts.ConstructInPlace();
        for (const auto& [alertType, alertError] : alertsNode->AsMap()) {
            result.Alerts->emplace(alertType, TYtError(alertError));
        }
    }

    return result;
}

TJobAttributes ParseJobAttributes(const TNode& node)
{
    const auto& mapNode = node.AsMap();
    TJobAttributes result;

    // Currently "get_job" returns "job_id" field and "list_jobs" returns "id" field.
    auto idNode = mapNode.FindPtr("id");
    if (!idNode) {
        idNode = mapNode.FindPtr("job_id");
    }
    if (idNode) {
        result.Id = GetGuid(idNode->AsString());
    }

    if (auto typeNode = mapNode.FindPtr("type")) {
        result.Type = FromString<EJobType>(typeNode->AsString());
    }
    if (auto stateNode = mapNode.FindPtr("state")) {
        result.State = FromString<EJobState>(stateNode->AsString());
    }
    if (auto addressNode = mapNode.FindPtr("address")) {
        result.Address = addressNode->AsString();
    }
    if (auto taskNameNode = mapNode.FindPtr("task_name")) {
        result.TaskName = taskNameNode->AsString();
    }
    if (auto startTimeNode = mapNode.FindPtr("start_time")) {
        result.StartTime = TInstant::ParseIso8601(startTimeNode->AsString());
    }
    if (auto finishTimeNode = mapNode.FindPtr("finish_time")) {
        result.FinishTime = TInstant::ParseIso8601(finishTimeNode->AsString());
    }
    if (auto progressNode = mapNode.FindPtr("progress")) {
        result.Progress = progressNode->AsDouble();
    }
    if (auto stderrSizeNode = mapNode.FindPtr("stderr_size")) {
        result.StderrSize = stderrSizeNode->AsUint64();
    }
    if (auto errorNode = mapNode.FindPtr("error")) {
        result.Error.ConstructInPlace(*errorNode);
    }
    if (auto briefStatisticsNode = mapNode.FindPtr("brief_statistics")) {
        result.BriefStatistics = *briefStatisticsNode;
    }
    if (auto inputPathsNode = mapNode.FindPtr("input_paths")) {
        const auto& inputPathNodesList = inputPathsNode->AsList();
        result.InputPaths.ConstructInPlace();
        result.InputPaths->reserve(inputPathNodesList.size());
        for (const auto& inputPathNode : inputPathNodesList) {
            TRichYPath path;
            Deserialize(path, inputPathNode);
            result.InputPaths->push_back(std::move(path));
        }
    }
    if (auto coreInfosNode = mapNode.FindPtr("core_infos")) {
        const auto& coreInfoNodesList = coreInfosNode->AsList();
        result.CoreInfos.ConstructInPlace();
        result.CoreInfos->reserve(coreInfoNodesList.size());
        for (const auto& coreInfoNode : coreInfoNodesList) {
            TCoreInfo coreInfo;
            coreInfo.ProcessId = coreInfoNode["process_id"].AsInt64();
            coreInfo.ExecutableName = coreInfoNode["executable_name"].AsString();
            if (coreInfoNode.HasKey("size")) {
                coreInfo.Size = coreInfoNode["size"].AsUint64();
            }
            if (coreInfoNode.HasKey("error")) {
                coreInfo.Error.ConstructInPlace(coreInfoNode["error"]);
            }
            result.CoreInfos->push_back(std::move(coreInfo));
        }
    }
    return result;
}

TCheckPermissionResponse ParseCheckPermissionResponse(const TNode& node)
{
    auto parseSingleResult = [] (const TNode::TMapType& node) {
        TCheckPermissionResult result;
        result.Action = ::FromString<ESecurityAction>(node.at("action").AsString());
        if (auto objectId = node.FindPtr("object_id")) {
            result.ObjectId = GetGuid(objectId->AsString());
        }
        if (auto objectName = node.FindPtr("object_name")) {
            result.ObjectName = objectName->AsString();
        }
        if (auto subjectId = node.FindPtr("subject_id")) {
            result.SubjectId = GetGuid(subjectId->AsString());
        }
        if (auto subjectName = node.FindPtr("subject_name")) {
            result.SubjectName = subjectName->AsString();
        }
        return result;
    };

    const auto& mapNode = node.AsMap();
    TCheckPermissionResponse result;
    static_cast<TCheckPermissionResult&>(result) = parseSingleResult(mapNode);
    if (auto columns = mapNode.FindPtr("columns")) {
        result.Columns.reserve(columns->AsList().size());
        for (const auto& columnNode : columns->AsList()) {
            result.Columns.push_back(parseSingleResult(columnNode.AsMap()));
        }
    }
    return result;
}

TRichYPath CanonizeYPath(
    const IRawClientPtr& rawClient,
    const TRichYPath& path)
{
    return CanonizeYPaths(rawClient, {path}).front();
}

TVector<TRichYPath> CanonizeYPaths(
    const IRawClientPtr& rawClient,
    const TVector<TRichYPath>& paths)
{
    auto batch = rawClient->CreateRawBatchRequest();

    TVector<NThreading::TFuture<TRichYPath>> futures;
    futures.reserve(paths.size());
    for (const auto& path : paths) {
        futures.push_back(batch->CanonizeYPath(path));
    }

    batch->ExecuteBatch();

    TVector<TRichYPath> result;
    result.reserve(futures.size());
    for (auto& future : futures) {
        result.push_back(future.ExtractValueSync());
    }
    return result;
}

NHttpClient::IHttpResponsePtr SkyShareTable(
    const TClientContext& context,
    const std::vector<TYPath>& tablePaths,
    const TSkyShareTableOptions& options)
{
    TMutationId mutationId;
    THttpHeader header("POST", "api/v1/share", /*IsApi*/ false);

    auto proxyName = context.ServerName.substr(0,  context.ServerName.find('.'));

    auto host = context.Config->SkynetApiHost;
    if (host == "") {
        host = "skynet." + proxyName + ".yt.yandex.net";
    }

    TSkyShareTableOptions patchedOptions = options;

    if (context.Config->Pool && !patchedOptions.Pool_) {
        patchedOptions.Pool(context.Config->Pool);
    }

    header.MergeParameters(SerializeParamsForSkyShareTable(proxyName, context.Config->Prefix, tablePaths, patchedOptions));
    TClientContext skyApiHost({.ServerName = host, .HttpClient = NHttpClient::CreateDefaultHttpClient()});

    return RequestWithoutRetry(skyApiHost, mutationId, header, "");
}

struct THttpRequestStream
    : public IOutputStream
{
public:
    THttpRequestStream(NHttpClient::IHttpRequestPtr request)
        : Request_(std::move(request))
        , Underlying_(Request_->GetStream())
    { }

private:
    void DoWrite(const void* buf, size_t len) override
    {
        Underlying_->Write(buf, len);
    }

    void DoFinish() override
    {
        Underlying_->Finish();
        Request_->Finish()->GetResponse();
    }

private:
    NHttpClient::IHttpRequestPtr Request_;
    IOutputStream* Underlying_;
};

struct THttpBufferedRequestStream
    : public IOutputStream
{
public:
    THttpBufferedRequestStream(NHttpClient::IHttpRequestPtr request, size_t bufferSize)
        : Request_(std::move(request))
        , Underlying_(std::make_unique<TBufferedOutput>(Request_->GetStream(), bufferSize))
    { }

private:
    void DoWrite(const void* buf, size_t len) override
    {
        Underlying_->Write(buf, len);
    }

    void DoFinish() override
    {
        Underlying_->Finish();
        Request_->Finish()->GetResponse();
    }

private:
    NHttpClient::IHttpRequestPtr Request_;
    std::unique_ptr<IOutputStream> Underlying_;
};

std::unique_ptr<IOutputStream> WriteTable(
    const TClientContext& context,
    const TTransactionId& transactionId,
    const TRichYPath& path,
    const TMaybe<TFormat>& format,
    const TTableWriterOptions& options)
{
    THttpHeader header("PUT", GetWriteTableCommand(context.Config->ApiVersion));
    header.AddTransactionId(transactionId);
    header.SetInputFormat(format);
    header.SetRequestCompression(ToString(context.Config->ContentEncoding));
    header.MergeParameters(FormIORequestParameters(path, options));

    TRequestConfig config;
    config.IsHeavy = true;
    auto request = StartRequestWithoutRetry(context, header, config);
    if (options.SingleHttpRequest_) {
        return std::make_unique<THttpBufferedRequestStream>(std::move(request), options.BufferSize_);
    }
    return std::make_unique<THttpRequestStream>(std::move(request));
}

std::unique_ptr<IOutputStream> WriteFile(
    const TClientContext& context,
    const TTransactionId& transactionId,
    const TRichYPath& path,
    const TFileWriterOptions& options)
{
    THttpHeader header("PUT", GetWriteFileCommand(context.Config->ApiVersion));
    header.AddTransactionId(transactionId);
    header.SetRequestCompression(ToString(context.Config->ContentEncoding));
    header.MergeParameters(FormIORequestParameters(path, options));

    TRequestConfig config;
    config.IsHeavy = true;
    auto request = StartRequestWithoutRetry(context, header, config);
    return std::make_unique<THttpRequestStream>(std::move(request));
}

TAuthorizationInfo WhoAmI(const TClientContext& context)
{
    TMutationId mutationId;
    THttpHeader header("GET", "auth/whoami", /*isApi*/ false);
    auto requestResult = RequestWithoutRetry(context, mutationId, header);
    TAuthorizationInfo result;

    NJson::TJsonValue jsonValue;
    bool ok = NJson::ReadJsonTree(requestResult->GetResponse(), &jsonValue, /*throwOnError*/ true);
    Y_ABORT_UNLESS(ok);
    result.Login = jsonValue["login"].GetString();
    result.Realm = jsonValue["realm"].GetString();
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDetail::NRawClient
