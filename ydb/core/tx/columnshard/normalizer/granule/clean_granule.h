#pragma once

#include <ydb/core/tx/columnshard/columnshard_schema.h>
#include <ydb/core/tx/columnshard/normalizer/abstract/abstract.h>

namespace NKikimr::NOlap {

class TCleanGranuleIdNormalizer: public TNormalizationController::INormalizerComponent {
private:
    using TBase = TNormalizationController::INormalizerComponent;

public:
    static TString GetClassNameStatic() {
        return ::ToString(ENormalizerSequentialId::CleanGranuleId);
    }

private:
    class TNormalizerResult;

    static inline INormalizerComponent::TFactory::TRegistrator<TCleanGranuleIdNormalizer> Registrator =
        INormalizerComponent::TFactory::TRegistrator<TCleanGranuleIdNormalizer>(GetClassNameStatic());

public:
    virtual std::optional<ENormalizerSequentialId> DoGetEnumSequentialId() const override {
        return ENormalizerSequentialId::CleanGranuleId;
    }

    virtual TString GetClassName() const override {
        return GetClassNameStatic();
    }

    TCleanGranuleIdNormalizer(const TNormalizationController::TInitContext& context)
        : TBase(context) {
    }

    virtual TConclusion<std::vector<INormalizerTask::TPtr>> DoInit(
        const TNormalizationController& controller, NTabletFlatExecutor::TTransactionContext& txc) override;
};

}   // namespace NKikimr::NOlap
