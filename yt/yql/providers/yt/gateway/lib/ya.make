LIBRARY()

SRCS(
    exec_ctx.h
    exec_ctx.cpp
    map_builder.h
    map_builder.cpp
    query_cache.cpp
    query_cache.h
    session.h
    session.cpp
    temp_files.cpp
    temp_files.h
    transaction_cache.cpp
    transaction_cache.h
    qb2.cpp
    qb2.h
    user_files.cpp
    user_files.h
    yt_helpers.cpp
    yt_helpers.h
)

PEERDIR(
    library/cpp/regex/pcre
    library/cpp/string_utils/url
    library/cpp/threading/future
    library/cpp/yson/node
    yt/cpp/mapreduce/client
    yt/cpp/mapreduce/common
    yt/cpp/mapreduce/interface
    yql/essentials/core/file_storage
    yql/essentials/public/issue
    yql/essentials/utils
    yql/essentials/utils/log
    yql/essentials/utils/threading
    yql/essentials/core/type_ann
    yql/essentials/providers/common/codec
    yql/essentials/providers/common/gateway
    yql/essentials/providers/common/metrics
    yt/yql/providers/yt/provider
    yt/yql/providers/yt/common
    yt/yql/providers/yt/lib/hash
    yt/yql/providers/yt/lib/res_pull
    yt/yql/providers/yt/lib/url_mapper
    yt/yql/providers/yt/lib/yson_helpers
)

YQL_LAST_ABI_VERSION()

END()
