#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/unittest/tests_data.h>
#include <ydb/library/actors/helpers/selfping_actor.h>
#include <util/stream/null.h>
#include <util/datetime/cputimer.h>
#include <util/system/compiler.h>
#include "hive_impl.h"
#include "balancer.h"

#ifdef NDEBUG
#define Ctest Cnull
#else
#define Ctest Cerr
#endif

using namespace NKikimr;
using namespace NHive;

namespace {

class TTestHive : public THive {
public:
    TTestHive(TTabletStorageInfo *info, const TActorId &tablet) : THive(info, tablet) {}

    template<typename F>
    void UpdateConfig(F func) {
        func(ClusterConfig);
        BuildCurrentConfig();
    }

    TBootQueue& GetBootQueue() {
        return BootQueue;
    }
};

} // namespace

using duration_nano_t = std::chrono::duration<ui64, std::nano>;
using duration_t = std::chrono::duration<double>;

duration_t GetBasePerformance() {
    duration_nano_t accm{};
    for (int i = 0; i < 1000000; ++i) {
        accm += duration_nano_t(NActors::MeasureTaskDurationNs());
    }
    return std::chrono::duration_cast<duration_t>(accm);
}

static double BASE_PERF = GetBasePerformance().count();

Y_UNIT_TEST_SUITE(THiveImplTest) {
    Y_UNIT_TEST(BootQueueSpeed) {
        TBootQueue bootQueue;
        static constexpr ui64 NUM_TABLETS = 1000000;

        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        THive hive(hiveStorage.Get(), TActorId());
        std::unordered_map<ui64, TLeaderTabletInfo> tablets;
        TProfileTimer timer;

        for (ui64 i = 0; i < NUM_TABLETS; ++i) {
            TLeaderTabletInfo& tablet = tablets.emplace(std::piecewise_construct, std::tuple<TTabletId>(i), std::tuple<TTabletId, THive&>(i, hive)).first->second;
            tablet.Weight = RandomNumber<double>();
            bootQueue.AddToBootQueue(tablet, 0UL);
        }

        double passed = timer.Get().SecondsFloat();
        Ctest << "Create = " << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
        UNIT_ASSERT(passed < 3 * BASE_PERF);
#else
        UNIT_ASSERT(passed < 1 * BASE_PERF);
#endif
#endif
        timer.Reset();

        double maxP = 100;
        std::vector<TBootQueue::TBootQueueRecord> records;
        records.reserve(NUM_TABLETS);
        unsigned i = 0;

        while (!bootQueue.Empty()) {
            auto record = bootQueue.PopFromBootQueue();
            UNIT_ASSERT(record.Priority <= maxP);
            maxP = record.Priority;
            UNIT_ASSERT(tablets.contains(record.TabletId));
            records.push_back(record);
            if (++i == NUM_TABLETS / 2) {
                // to test both modes
                bootQueue.IncludeWaitQueue();
            }
        }
        bootQueue.ExcludeWaitQueue();

        i = 0;
        for (auto& record : records) {
            if (++i % 3 == 0) {
                bootQueue.AddToBootQueue(record);
            } else {
                bootQueue.AddToWaitQueue(record);
            }
        }

        passed = timer.Get().SecondsFloat();
        Ctest << "Process = " << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
        UNIT_ASSERT(passed < 10 * BASE_PERF);
#else
        UNIT_ASSERT(passed < 2 * BASE_PERF);
#endif
#endif

        timer.Reset();

        bootQueue.IncludeWaitQueue();
        while (!bootQueue.Empty()) {
            bootQueue.PopFromBootQueue();
        }
        bootQueue.ExcludeWaitQueue();

        passed = timer.Get().SecondsFloat();
        Ctest << "Move = " << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
        UNIT_ASSERT(passed < 2 * BASE_PERF);
#else
        UNIT_ASSERT(passed < 0.1 * BASE_PERF);
#endif
#endif
    }

    Y_UNIT_TEST(BalancerSpeedAndDistribution) {
        static constexpr ui64 NUM_TABLETS = 1000000;
        static constexpr ui64 NUM_BUCKETS = 16;

        auto CheckSpeedAndDistribution = [](
            std::unordered_map<ui64, TLeaderTabletInfo>& allTablets,
            std::function<void(std::vector<TTabletInfo*>::iterator, std::vector<TTabletInfo*>::iterator, EResourceToBalance)> func,
            EResourceToBalance resource) -> void {

            std::vector<TTabletInfo*> tablets;
            for (auto& [id, tab] : allTablets) {
                tablets.emplace_back(&tab);
            }

            TProfileTimer timer;

            func(tablets.begin(), tablets.end(), resource);

            double passed = timer.Get().SecondsFloat();

            Ctest << "Time=" << passed << Endl;
#ifndef _san_enabled_
#ifndef NDEBUG
            UNIT_ASSERT(passed < 1 * BASE_PERF);
#else
            UNIT_ASSERT(passed < 1 * BASE_PERF);
#endif
#endif
            std::vector<double> buckets(NUM_BUCKETS, 0);
            size_t revs = 0;
            double prev = 0;
            for (size_t n = 0; n < tablets.size(); ++n) {
                double weight = tablets[n]->GetWeight(resource);
                buckets[n / (NUM_TABLETS / NUM_BUCKETS)] += weight;
                if (n != 0 && weight >= prev) {
                    ++revs;
                }
                prev = weight;
            }

            Ctest << "Indirection=" << revs * 100 / NUM_TABLETS << "%" << Endl;

            Ctest << "Distribution=";
            for (double v : buckets) {
                Ctest << Sprintf("%.2f", v) << " ";
            }
            Ctest << Endl;

            /*char bars[] = " ▁▂▃▄▅▆▇█";
            double mx = *std::max_element(buckets.begin(), buckets.end());
            for (double v : buckets) {
                Ctest << bars[int(ceil(v / mx)) * 8];
            }
            Ctest << Endl;*/

            /*prev = NUM_TABLETS;
            for (double v : buckets) {
                UNIT_ASSERT(v < prev);
                prev = v;
            }*/

            std::sort(tablets.begin(), tablets.end());
            auto unique_end = std::unique(tablets.begin(), tablets.end());
            Ctest << "Duplicates=" << tablets.end() - unique_end << Endl;

            UNIT_ASSERT(tablets.end() - unique_end == 0);
        };

        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        THive hive(hiveStorage.Get(), TActorId());
        std::unordered_map<ui64, TLeaderTabletInfo> allTablets;

        for (ui64 i = 0; i < NUM_TABLETS; ++i) {
            TLeaderTabletInfo& tablet = allTablets.emplace(std::piecewise_construct, std::tuple<TTabletId>(i), std::tuple<TTabletId, THive&>(i, hive)).first->second;
            tablet.GetMutableResourceValues().SetMemory(RandomNumber<double>());
        }

        Ctest << "HIVE_TABLET_BALANCE_STRATEGY_HEAVIEST" << Endl;
        CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_HEAVIEST>, EResourceToBalance::Memory);

        //Ctest << "HIVE_TABLET_BALANCE_STRATEGY_OLD_WEIGHTED_RANDOM" << Endl;
        //CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_OLD_WEIGHTED_RANDOM>);

        Ctest << "HIVE_TABLET_BALANCE_STRATEGY_WEIGHTED_RANDOM" << Endl;
        CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_WEIGHTED_RANDOM>, EResourceToBalance::Memory);

        Ctest << "HIVE_TABLET_BALANCE_STRATEGY_RANDOM" << Endl;
        CheckSpeedAndDistribution(allTablets, BalanceTablets<NKikimrConfig::THiveConfig::HIVE_TABLET_BALANCE_STRATEGY_RANDOM>, EResourceToBalance::Memory);
    }

    Y_UNIT_TEST(TestShortTabletTypes) {
        // This asserts we don't have different tablet types with same short name
        // In a world with constexpr maps this could have been a static_assert...
        UNIT_ASSERT_VALUES_EQUAL(TABLET_TYPE_SHORT_NAMES.size(), TABLET_TYPE_BY_SHORT_NAME.size());
    }

    Y_UNIT_TEST(TestStDev) {
        using TSingleResource = std::tuple<double>;

        TVector<TSingleResource> values(100, 50.0 / 1'000'000);
        values.front() = 51.0 / 1'000'000;

        double stDev1 = std::get<0>(GetStDev(values));

        std::swap(values.front(), values.back());

        double stDev2 = std::get<0>(GetStDev(values));

        double expectedStDev = sqrt(0.9703) / 1'000'000;

        UNIT_ASSERT_DOUBLES_EQUAL(expectedStDev, stDev1, 1e-6);
        UNIT_ASSERT_VALUES_EQUAL(stDev1, stDev2);
    }

    Y_UNIT_TEST(BootQueueConfigurePriorities) {
        auto hiveStorage = MakeIntrusive<TTabletStorageInfo>();
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());

        // Emulate initial BuildCurrentConfig call
        hive.UpdateConfig([](NKikimrConfig::THiveConfig&){});

        TFollowerGroup followerGroup(hive);

        // Part 1: Test default priorities with empty configuration
        {
            TLeaderTabletInfo ssTablet(1UL, hive);
            ssTablet.SetType(TTabletTypes::SchemeShard);
            hive.AddToBootQueue(&ssTablet);

            TLeaderTabletInfo dummyTablet(2UL, hive);
            dummyTablet.SetType(TTabletTypes::Dummy);
            hive.AddToBootQueue(&dummyTablet);

            TFollowerTabletInfo dummyFollowerTablet(dummyTablet, 3UL, followerGroup);
            hive.AddToBootQueue(&dummyFollowerTablet);

            auto& bootQueue = hive.GetBootQueue();
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 3);

            // Priorities should follow defaults
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 3.0); // SchemeShard default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 1.0); // Leader default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 0.0); // Follower default

            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 0);
        }

        // Part 2: Configure custom priorities for known and unknown types
        {
            constexpr double SS_BOOT_PRIORITY = 2.5;
            constexpr double DUMMY_BOOT_PRIORITY = 0.5;

            hive.UpdateConfig([&](NKikimrConfig::THiveConfig& config) {
                auto* ssItem = config.AddTabletTypeToBootPriority();
                ssItem->SetTabletType(TTabletTypes::SchemeShard);
                ssItem->SetPriority(SS_BOOT_PRIORITY);

                auto* dummyItem = config.AddTabletTypeToBootPriority();
                dummyItem->SetTabletType(TTabletTypes::Dummy);
                dummyItem->SetPriority(DUMMY_BOOT_PRIORITY);
            });

            TLeaderTabletInfo configuredSsTablet(4UL, hive);
            configuredSsTablet.SetType(TTabletTypes::SchemeShard);
            hive.AddToBootQueue(&configuredSsTablet);

            TLeaderTabletInfo configuredDummyTablet(5UL, hive);
            configuredDummyTablet.SetType(TTabletTypes::Dummy);
            hive.AddToBootQueue(&configuredDummyTablet);

            TFollowerTabletInfo configuredDummyFollowerTablet(configuredDummyTablet, 6UL, followerGroup);
            hive.AddToBootQueue(&configuredDummyFollowerTablet);

            auto& bootQueue = hive.GetBootQueue();
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 3);

            // Priorities should use configured values
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, SS_BOOT_PRIORITY); // Configured SchemeShard
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, DUMMY_BOOT_PRIORITY); // Configured Dummy
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 0.0); // Follower default (should not change)

            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 0);
        }

        // Part 3: Clear configuration and verify defaults again
        {
            hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
                config.ClearTabletTypeToBootPriority();
            });

            TLeaderTabletInfo ssTabletAfterClear(7UL, hive);
            ssTabletAfterClear.SetType(TTabletTypes::SchemeShard);
            hive.AddToBootQueue(&ssTabletAfterClear);

            TLeaderTabletInfo dummyTabletAfterClear(8UL, hive);
            dummyTabletAfterClear.SetType(TTabletTypes::Dummy);
            hive.AddToBootQueue(&dummyTabletAfterClear);

            TFollowerTabletInfo dummyFollowerTabletAfterClear(dummyTabletAfterClear, 9UL, followerGroup);
            hive.AddToBootQueue(&dummyFollowerTabletAfterClear);

            auto& bootQueue = hive.GetBootQueue();
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 3);

            // Priorities should revert to defaults
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 3.0); // SchemeShard default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 1.0); // Leader default
            UNIT_ASSERT_VALUES_EQUAL(bootQueue.PopFromBootQueue().Priority, 0.0); // Follower default

            UNIT_ASSERT_VALUES_EQUAL(bootQueue.Size(), 0);
        }
    }
}

Y_UNIT_TEST_SUITE(TCutHistoryRestrictions) {

    Y_UNIT_TEST(BasicTest) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("DataShard,Coordinator");
            config.SetCutHistoryDenyList("GraphShard");
        });
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::GraphShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::Hive));
    }

    Y_UNIT_TEST(EmptyAllowList) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("");
            config.SetCutHistoryDenyList("GraphShard");
        });
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::GraphShard));
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::Hive));
    }

    Y_UNIT_TEST(EmptyDenyList) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("DataShard,Coordinator");
            config.SetCutHistoryDenyList("");
        });
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::GraphShard));
    }

    Y_UNIT_TEST(SameTabletInBothLists) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("DataShard,Coordinator");
            config.SetCutHistoryDenyList("SchemeShard,DataShard");
        });
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::SchemeShard));
        UNIT_ASSERT(!hive.IsCutHistoryAllowed(TTabletTypes::Hive));
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::Coordinator));
    }

    Y_UNIT_TEST(BothListsEmpty) {
        TIntrusivePtr<TTabletStorageInfo> hiveStorage = new TTabletStorageInfo;
        hiveStorage->TabletType = TTabletTypes::Hive;
        TTestHive hive(hiveStorage.Get(), TActorId());
        hive.UpdateConfig([](NKikimrConfig::THiveConfig& config) {
            config.SetCutHistoryAllowList("");
            config.SetCutHistoryDenyList("");
        });
        UNIT_ASSERT(hive.IsCutHistoryAllowed(TTabletTypes::DataShard));
    }
}
