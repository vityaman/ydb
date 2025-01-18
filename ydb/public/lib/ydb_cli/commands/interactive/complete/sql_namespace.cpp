#include "sql_namespace.h"

#include <util/charset/utf8.h>

namespace NSQLComplete {

    using NThreading::MakeFuture;

    TFuture<TYQLNamesList> TYQLNamespace::GetNamesStartingWith(TStringBuf prefix) {
        return GetNames().Apply([prefix = ToLowerUTF8(TYQLName(prefix))](auto&& future) {
            TYQLNamesList names = future.GetValue();
            auto removed = std::ranges::remove_if(names, [&](const TYQLName& name) {
                return !ToLowerUTF8(name).StartsWith(prefix);
            });
            names.erase(std::begin(removed), std::end(removed));
            return names;
        });
    }

    TYQLKeywords::TYQLKeywords(TYQLNamesList keywords)
        : Keywords(std::move(keywords))
    {
    }

    TFuture<TYQLNamesList> TYQLKeywords::GetNames() {
        return MakeFuture(Keywords);
    }

} // namespace NSQLComplete
