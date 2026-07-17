#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace emberstore
{
// A small typed query over an in-memory snapshot of documents — Firestore-
// shaped (where / orderBy / limit, chained, then get), but filtered by
// predicate and ordered by projection rather than field-path strings, so it
// stays type-safe. Built over the whole collection each call; fine for the
// modest collections this store targets.
template <typename T>
class Query
{
public:
    explicit Query(std::vector<T> rowsToUse)
        : rows(std::move(rowsToUse))
    {
    }

    // Keep only the documents `predicate` accepts.
    Query& where(std::function<bool(const T&)> predicate)
    {
        rows.erase(std::remove_if(rows.begin(),
                                  rows.end(),
                                  [&](const T& r) { return !predicate(r); }),
                   rows.end());
        return *this;
    }

    // Sort by the value `projection` pulls from each document — a lambda or a
    // member pointer (`orderBy(&Profile::level)`).
    template <typename Projection>
    Query& orderBy(Projection projection, bool ascending = true)
    {
        std::stable_sort(rows.begin(),
                         rows.end(),
                         [&](const T& a, const T& b)
                         {
                             return ascending ? std::invoke(projection, a)
                                                    < std::invoke(projection, b)
                                              : std::invoke(projection, b)
                                                    < std::invoke(projection, a);
                         });
        return *this;
    }

    Query& limit(std::size_t count)
    {
        if (rows.size() > count)
            rows.resize(count);
        return *this;
    }

    std::vector<T> get() const { return rows; }

    std::optional<T> first() const
    {
        if (rows.empty())
            return std::nullopt;
        return rows.front();
    }

    std::size_t count() const { return rows.size(); }

private:
    std::vector<T> rows;
};
} // namespace emberstore
