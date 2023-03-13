/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "init.hh"
#include "gms/failure_detector.hh"
#include "utils/to_string.hh"
#include "gms/inet_address.hh"
#include "utils/fb_utilities.hh"
#include "seastarx.hh"
#include "db/config.hh"

#include <boost/algorithm/string/trim.hpp>

logging::logger startlog("init");

std::set<gms::inet_address> get_seeds_from_db_config(const db::config& cfg) {
    auto preferred = cfg.listen_interface_prefer_ipv6() ? std::make_optional(net::inet_address::family::INET6) : std::nullopt;
    auto family = cfg.enable_ipv6_dns_lookup() || preferred ? std::nullopt : std::make_optional(net::inet_address::family::INET);
    const auto listen = gms::inet_address::lookup(cfg.listen_address(), family).get0();
    auto seed_provider = cfg.seed_provider();

    std::set<gms::inet_address> seeds;
    if (seed_provider.parameters.contains("seeds")) {
        size_t begin = 0;
        size_t next = 0;
        sstring seeds_str = seed_provider.parameters.find("seeds")->second;
        while (begin < seeds_str.length() && begin != (next=seeds_str.find(",",begin))) {
            auto seed = boost::trim_copy(seeds_str.substr(begin,next-begin));
            try {
                seeds.emplace(gms::inet_address::lookup(seed, family, preferred).get0());
            } catch (...) {
                startlog.error("Bad configuration: invalid value in 'seeds': '{}': {}", seed, std::current_exception());
                throw bad_configuration_error();
            }
            begin = next+1;
        }
    }
    if (seeds.empty()) {
        seeds.emplace(gms::inet_address("127.0.0.1"));
    }
    auto broadcast_address = utils::fb_utilities::get_broadcast_address();
    startlog.info("seeds={}, listen_address={}, broadcast_address={}",
            to_string(seeds), listen, broadcast_address);
    if (broadcast_address != listen && seeds.contains(listen)) {
        startlog.error("Use broadcast_address instead of listen_address for seeds list");
        throw std::runtime_error("Use broadcast_address for seeds list");
    }
    if (!cfg.replace_node_first_boot().empty() && seeds.contains(broadcast_address)) {
        startlog.error("Bad configuration: replace-node-first-boot is not allowed for seed nodes");
        throw bad_configuration_error();
    }
    if ((!cfg.replace_address_first_boot().empty() || !cfg.replace_address().empty()) && seeds.contains(broadcast_address)) {
        startlog.error("Bad configuration: replace-address and replace-address-first-boot are not allowed for seed nodes");
        throw bad_configuration_error();
    }

    return seeds;
}
