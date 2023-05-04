/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <concepts>
#include <fmt/core.h>
#include <cstdint>
#include <compare>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <variant>
#include <type_traits>
#include <boost/range/adaptors.hpp>
#include <boost/regex.hpp>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include "utils/UUID.hh"
#include "utils/UUID_gen.hh"
#include "types/types.hh"
#include "utils/UUID_gen.hh"

namespace sstables {

class generation_type {
public:
    using int_t = int64_t;

private:
    utils::UUID _value;

public:
    // construct an "empty" generation, whose value is a null UUID
    generation_type() = default;

    // use zero as the timestamp to differentiate from the regular timeuuid,
    // and use the least_sig_bits to encode the value of generation identifier.
    explicit constexpr generation_type(int_t value) noexcept
        : _value(utils::UUID_gen::create_time(std::chrono::milliseconds::zero()), value) {}
    explicit constexpr generation_type(utils::UUID value) noexcept
        : _value(value) {}
    static generation_type from_string(const std::string& s) {
        int64_t int_value;
        if (auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), int_value);
            ec == std::errc() && ptr == s.data() + s.size()) {
            return generation_type(int_value);
        } else {
            boost::regex pattern("([[:alnum:]]{4})_([[:alnum:]]{4})_([[:alnum:]]{5})([[:alnum:]]{13})");
            boost::smatch match;
            if (!boost::regex_match(s, match, pattern)) {
                throw std::invalid_argument(fmt::format("invalid UUID: {}", s));
            }
            utils::UUID_gen::decimicroseconds timestamp = {};
            timestamp += std::chrono::days{std::stol(match[0], nullptr, 36)};
            timestamp += std::chrono::seconds{std::stol(match[1], nullptr, 36)};
            timestamp += ::utils::UUID_gen::decimicroseconds{std::stoul(match[2], nullptr, 36)};
            int64_t lsb = std::stoull(match[3], nullptr, 36);
            return generation_type{utils::UUID_gen::get_time_UUID_raw(timestamp, lsb)};
        }
    }
    template<typename T>
    constexpr T value() const noexcept {
        if constexpr(std::same_as<T, utils::UUID>) {
            assert(is_uuid_based());
            return utils::UUID(*this);
        } else {
            return int64_t(*this);
        }
    }
    // return true if the generation holds a valid id
    explicit operator bool() const noexcept {
        return bool(_value);
    }
    explicit constexpr operator int64_t() const noexcept {
        assert(!is_uuid_based());
        return _value.get_least_significant_bits();
    }
    explicit constexpr operator utils::UUID() const noexcept {
        assert(is_uuid_based());
        return _value;
    }
    // convert to data_value
    //
    // this function is used when performing queries to SSTABLES_REGISTRY in
    // the "system_keyspace", since its "generation" column cannot be a variant
    // of bigint and timeuuid, we need to use a single value to represent these
    // two types, and single value should allow us to tell the type of the
    // original value of generation identifier, so we can convert the value back
    // to the generation when necessary without losing its type information.
    // since the timeuuid always encodes the timestamp in its MSB, and the timestamp
    // should always be greater than zero, we use this fact to tell a regular
    // timeuuid from a timeuuid converted from a bigint -- we just use zero
    // for its timestamp of the latter.
    explicit operator data_value() const noexcept {
        return _value;
    }
    constexpr bool is_uuid_based() const noexcept {
        // if the value of generation_type should be an int64_t, its timestamp
        // must be zero, and the least significant bits is used to encode the
        // value of the int64_t.
        return _value.timestamp() != 0;
    }

    constexpr bool operator==(const generation_type& other) const noexcept { return _value == other._value; }
    constexpr std::strong_ordering operator<=>(const generation_type& other) const noexcept { return _value <=> other._value; }
};

constexpr generation_type generation_from_value(generation_type::int_t value) {
    return generation_type{value};
}

template <std::ranges::range Range, typename Target = std::vector<sstables::generation_type>>
Target generations_from_values(const Range& values) {
    return boost::copy_range<Target>(values | boost::adaptors::transformed([] (auto value) {
        return generation_type(value);
    }));
}

template <typename Target = std::vector<sstables::generation_type>>
Target generations_from_values(std::initializer_list<generation_type::int_t> values) {
    return boost::copy_range<Target>(values | boost::adaptors::transformed([] (auto value) {
        return generation_type(value);
    }));
}

class sstable_generation_generator {
    // We still want to do our best to keep the generation numbers shard-friendly.
    // Each destination shard will manage its own generation counter.
    //
    // operator() is called by multiple shards in parallel when performing reshard,
    // so we have to use atomic<> here.
    using int_t = sstables::generation_type::int_t;
    int_t _last_generation;
    static int_t base_generation(int_t highest_generation) {
        // get the base generation so we can increment it by smp::count without
        // conflicting with other shards
        return highest_generation - highest_generation % seastar::smp::count + seastar::this_shard_id();
    }
public:
    explicit sstable_generation_generator(int64_t last_generation)
        : _last_generation(base_generation(last_generation)) {}
    void update_known_generation(int64_t generation) {
        if (generation > _last_generation) {
            _last_generation = generation;
        }
    }
    // TODO: remove the default value of uuid_identifier, and use related configuration
    sstables::generation_type operator()(bool uuid_identifier = false) {
        if (uuid_identifier) {
            return generation_type(utils::UUID_gen::get_time_UUID());
        }
        // each shard has its own "namespace" so we increment the generation id
        // by smp::count to avoid name confliction of sstables
        _last_generation += seastar::smp::count;
        return sstables::generation_from_value(_last_generation);
    }
};

} //namespace sstables

namespace std {
template <>
struct hash<sstables::generation_type> {
    size_t operator()(const sstables::generation_type& generation) const noexcept {
        if (generation.is_uuid_based()) {
            return hash<utils::UUID>{}(utils::UUID(generation));
        } else {
            return hash<int64_t>{}(int64_t(generation));
        }
    }
};

// for min_max_tracker
template <>
struct numeric_limits<sstables::generation_type> : public numeric_limits<sstables::generation_type::int_t> {
    static constexpr sstables::generation_type min() noexcept {
        return sstables::generation_type{numeric_limits<sstables::generation_type::int_t>::min()};
    }
    static constexpr sstables::generation_type max() noexcept {
        return sstables::generation_type{numeric_limits<sstables::generation_type::int_t>::max()};
    }
};
} //namespace std

template <>
struct fmt::formatter<sstables::generation_type> : fmt::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(const sstables::generation_type& generation, FormatContext& ctx) const {
        if (generation.is_uuid_based()) {
            // This matches the way how Cassandra formats UUIDBasedSSTableId, but we
            // don't have to. just don't want to use "-" as the delimeter in UUID, as
            // "-" is already used to split different parts in a SStable filename like
            // "nb-1-big-Data.db".
            const auto uuid = generation.value<::utils::UUID>();
            auto timestamp = ::utils::UUID_gen::decimicroseconds(uuid.timestamp());

            char days_buf[4] = {};
            auto days = std::chrono::duration_cast<std::chrono::days>(timestamp);
            timestamp -= days;
            char* days_end = std::to_chars(std::begin(days_buf), std::end(days_buf),
                                           days.count(), 36).ptr;

            char secs_buf[4] = {};
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(timestamp);
            timestamp -= secs;
            char* secs_end = std::to_chars(std::begin(secs_buf), std::end(secs_buf),
                                           secs.count(), 36).ptr;

            char decimicro_buf[5] = {};
            char* decimicro_end = std::to_chars(std::begin(decimicro_buf), std::end(decimicro_buf),
                                                timestamp.count(), 36).ptr;

            char lsb_buf[13] = {};
            char* lsb_end = std::to_chars(std::begin(lsb_buf), std::end(lsb_buf),
                                          static_cast<uint64_t>(uuid.get_least_significant_bits()), 36).ptr;

            return fmt::format_to(ctx.out(), "{:0>4}_{:0>4}_{:0>5}{:0>13}",
                                  std::string_view(days_buf, days_end),
                                  std::string_view(secs_buf, secs_end),
                                  std::string_view(decimicro_buf, decimicro_end),
                                  std::string_view(lsb_buf, lsb_end));
        } else {
            return fmt::format_to(ctx.out(), "{}", int64_t(generation));
        }
    }
};
