/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <fmt/core.h>
#include <cstdint>
#include <compare>
#include <limits>
#include <iostream>
#include <type_traits>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>

namespace sstables {

class generation_type {
    int64_t _value;
public:
    generation_type() = delete;

    explicit constexpr generation_type(int64_t value) noexcept: _value(value) {}
    constexpr int64_t value() const noexcept { return _value; }

    constexpr bool operator==(const generation_type& other) const noexcept { return _value == other._value; }
    constexpr std::strong_ordering operator<=>(const generation_type& other) const noexcept { return _value <=> other._value; }
};

constexpr generation_type generation_from_value(int64_t value) {
    return generation_type{value};
}
constexpr int64_t generation_value(generation_type generation) {
    return generation.value();
}

struct is_shared {
    using yes = std::true_type;
    using no = std::false_type;
};

template<typename IsShared>
class sstable_generation_generator {
    // We still want to do our best to keep the generation numbers shard-friendly.
    // Each destination shard will manage its own generation counter.
    //
    // operator() is called by multiple shards in parallel when performing reshard,
    // so we have to use atomic<> here.
    std::conditional_t<IsShared::value, std::atomic<int64_t>, int64_t> _last_generation;
public:
    explicit sstable_generation_generator(int64_t last_generation)
        : _last_generation(last_generation) {}
    sstables::generation_type operator()(seastar::shard_id shard) {
        int64_t v;
        if constexpr (IsShared::value) {
            v = _last_generation.fetch_add(seastar::smp::count, std::memory_order_relaxed);
        } else {
            v = (_last_generation += seastar::smp::count);
        }
        return sstables::generation_from_value(v + shard);
    }
};

} //namespace sstables

namespace std {
template <>
struct hash<sstables::generation_type> {
    size_t operator()(const sstables::generation_type& generation) const noexcept {
        return hash<int64_t>{}(generation.value());
    }
};

// for min_max_tracker
template <>
struct numeric_limits<sstables::generation_type> : public numeric_limits<int64_t> {
    static constexpr sstables::generation_type min() noexcept {
        return sstables::generation_type{numeric_limits<int64_t>::min()};
    }
    static constexpr sstables::generation_type max() noexcept {
        return sstables::generation_type{numeric_limits<int64_t>::max()};
    }
};
} //namespace std

template <>
struct fmt::formatter<sstables::generation_type> : fmt::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(const sstables::generation_type& generation, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{}", generation.value());
    }
};
