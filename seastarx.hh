/*
 * Copyright (C) 2017-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <boost/asio/ip/address_v4.hpp>  // avoid conflict between ::socket and seastar::socket
#include <fmt/core.h>

namespace seastar {

template <typename T>
class shared_ptr;

template <typename T, typename... A>
shared_ptr<T> make_shared(A&&... a);

template <typename char_type, typename Size, Size max_size, bool NulTerminatee>
class basic_sstring;

template <typename... A>
basic_sstring<char, uint32_t, 15, true>
format(fmt::format_string<A...> fmt, A&&... a);

}


using namespace seastar;
using seastar::shared_ptr;
using seastar::make_shared;
using seastar::format;
