/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/core/future.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/abort_source.hh>
#include "log.hh"
#include "seastarx.hh"
#include <boost/program_options.hpp>

namespace db {
class extensions;
class seed_provider_type;
class config;
namespace view {
class view_update_generator;
}
}

namespace gms {
class feature_service;
class inet_address;
}

extern logging::logger startlog;

class bad_configuration_error : public std::exception {};

std::set<gms::inet_address> get_seeds_from_db_config(const db::config& cfg);
