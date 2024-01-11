/*
 * Copyright (C) 2014-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Some thrift headers include other files from within namespaces,
// which is totally broken.  Include those files here to avoid
// breakage:
#include <sys/param.h>
// end thrift workaround
#include "Cassandra.h"
#include <seastar/core/distributed.hh>
#include <seastar/core/coroutine.hh>
#include "replica/database.hh" // for database::get_version()
#include "data_dictionary/data_dictionary.hh"
#include <seastar/core/sstring.hh>
#include <seastar/core/print.hh>
#include "mutation/frozen_mutation.hh"
#include "utils/UUID_gen.hh"
#include <thrift/protocol/TBinaryProtocol.h>
#include <boost/move/iterator.hpp>
#include "db/marshal/type_parser.hh"
#include "service/migration_manager.hh"
#include "service/storage_proxy.hh"
#include "utils/class_registrator.hh"
#include "noexcept_traits.hh"
#include "schema/schema_registry.hh"
#include "thrift/utils.hh"
#include "schema/schema_builder.hh"
#include "thrift/thrift_validation.hh"
#include "service/storage_service.hh"
#include "service/query_state.hh"
#include "cql3/query_processor.hh"
#include "cql3/column_identifier.hh"
#include "timeout_config.hh"
#include "mutation/mutation.hh"
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/uniqued.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/indirected.hpp>
#include "query-result-reader.hh"
#include "thrift/server.hh"
#include "db/config.hh"
#include "locator/abstract_replication_strategy.hh"

#include <fmt/ostream.h>

#ifdef THRIFT_USES_BOOST
namespace thrift_fn = tcxx;
#else
namespace thrift_fn = std;
#endif

using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
namespace thrift_transport = ::apache::thrift::transport;
using namespace ::apache::thrift::async;

using namespace  ::cassandra;

using namespace thrift;

template <> struct fmt::formatter<cassandra::ConsistencyLevel::type> : fmt::ostream_formatter {};

class unimplemented_exception : public std::exception {
public:
    virtual const char* what() const throw () override { return "sorry, not implemented"; }
};

void pass_unimplemented(const thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)>& exn_cob) {
    exn_cob(::apache::thrift::TDelayedException::delayException(unimplemented_exception()));
}

class delayed_exception_wrapper : public ::apache::thrift::TDelayedException {
    std::exception_ptr _ex;
public:
    delayed_exception_wrapper(std::exception_ptr ex) : _ex(std::move(ex)) {}
    virtual void throw_it() override {
        // Thrift auto-wraps unexpected exceptions (those not derived from TException)
        // with a TException, but with a fairly bad what().  So detect this, and
        // provide our own TException with a better what().
        try {
            std::rethrow_exception(std::move(_ex));
        } catch (const ::apache::thrift::TException&) {
            // It's an expected exception, so assume the message
            // is fine.  Also, we don't want to change its type.
            throw;
        } catch (no_such_class& nc) {
            throw make_exception<InvalidRequestException>(nc.what());
        } catch (marshal_exception& me) {
            throw make_exception<InvalidRequestException>(me.what());
        } catch (exceptions::already_exists_exception& ae) {
            throw make_exception<InvalidRequestException>(ae.what());
        } catch (exceptions::configuration_exception& ce) {
            throw make_exception<InvalidRequestException>(ce.what());
        } catch (exceptions::invalid_request_exception& ire) {
            throw make_exception<InvalidRequestException>(ire.what());
        } catch (data_dictionary::no_such_column_family& nocf) {
            throw make_exception<InvalidRequestException>(nocf.what());
        } catch (data_dictionary::no_such_keyspace&) {
            throw NotFoundException();
        } catch (exceptions::syntax_exception& se) {
            throw make_exception<InvalidRequestException>("syntax error: {}", se.what());
        } catch (exceptions::authentication_exception& ae) {
            throw make_exception<AuthenticationException>(ae.what());
        } catch (exceptions::unauthorized_exception& ue) {
            throw make_exception<AuthorizationException>(ue.what());
        } catch (std::exception& e) {
            // Unexpected exception, wrap it
            throw ::apache::thrift::TException(std::string("Internal server error: ") + e.what());
        } catch (...) {
            // Unexpected exception, wrap it, unfortunately without any info
            throw ::apache::thrift::TException("Internal server error");
        }
    }
};

template <typename Func, typename T>
void
with_cob(thrift_fn::function<void (const T& ret)>&& cob,
        thrift_fn::function<void (::apache::thrift::TDelayedException* _throw)>&& exn_cob,
        Func&& func) {
    // then_wrapped() terminates the fiber by calling one of the cob objects
    (void)futurize_invoke([func = std::forward<Func>(func)] {
        return noexcept_movable<T>::wrap(func());
    }).then_wrapped([cob = std::move(cob), exn_cob = std::move(exn_cob)] (auto&& f) {
        try {
            cob(noexcept_movable<T>::unwrap(f.get0()));
        } catch (...) {
            delayed_exception_wrapper dew(std::current_exception());
            exn_cob(&dew);
        }
    });
}

template <typename Func>
void
with_cob(thrift_fn::function<void ()>&& cob,
        thrift_fn::function<void (::apache::thrift::TDelayedException* _throw)>&& exn_cob,
        Func&& func) {
    // then_wrapped() terminates the fiber by calling one of the cob objects
    (void)futurize_invoke(func).then_wrapped([cob = std::move(cob), exn_cob = std::move(exn_cob)] (future<> f) {
        try {
            f.get();
            cob();
        } catch (...) {
            delayed_exception_wrapper dew(std::current_exception());
            exn_cob(&dew);
        }
    });
}

template <typename Func>
void
with_exn_cob(thrift_fn::function<void (::apache::thrift::TDelayedException* _throw)>&& exn_cob, Func&& func) {
    // then_wrapped() terminates the fiber by calling one of the cob objects
    (void)futurize_invoke(func).then_wrapped([exn_cob = std::move(exn_cob)] (future<> f) {
        try {
            f.get();
        } catch (...) {
            delayed_exception_wrapper dew(std::current_exception());
            exn_cob(&dew);
       }
    });
}

std::string bytes_to_string(bytes_view v) {
    return { reinterpret_cast<const char*>(v.begin()), v.size() };
}

std::string bytes_to_string(query::result_bytes_view v) {
    std::string str;
    str.reserve(v.size_bytes());
    using boost::range::for_each;
    for_each(v, [&] (bytes_view fragment) {
        auto view = std::string_view(reinterpret_cast<const char*>(fragment.data()), fragment.size());
        str.insert(str.end(), view.begin(), view.end());
    });
    return str;
}

std::string bytes_to_string(managed_bytes_view v) {
    std::string str;
    str.reserve(v.size_bytes());
    for (auto fragment : fragment_range(v)) {
        auto view = std::string_view(reinterpret_cast<const char*>(fragment.data()), fragment.size());
        str.insert(str.end(), view.begin(), view.end());
    }
    return str;
}

namespace thrift {
template<typename T>
concept Aggregator =
    requires() { typename T::type; }
    && requires(T aggregator, typename T::type* aggregation, const bytes& name, const query::result_atomic_cell_view& cell) {
        { aggregator.on_column(aggregation, name, cell) } -> std::same_as<void>;
    };
}

enum class query_order { no, yes };

class thrift_handler : public CassandraCobSvIf {
    data_dictionary::database _db;
    distributed<cql3::query_processor>& _query_processor;
    sharded<service::storage_service>& _ss;
    sharded<service::storage_proxy>& _proxy;
    ::timeout_config _timeout_config;
    service::client_state _client_state;
    service::query_state _query_state;
    service_permit& _current_permit;
private:
    template <typename Cob, typename Func>
    void
    with_schema(Cob&& cob,
                thrift_fn::function<void (::apache::thrift::TDelayedException* _throw)>&& exn_cob,
                const std::string& cf,
                Func&& func) {
        with_cob(std::move(cob), std::move(exn_cob), [this, &cf, func = std::move(func)] {
            auto schema = lookup_schema(_db, current_keyspace(), cf);
            return func(std::move(schema));
        });
    }
public:
    explicit thrift_handler(data_dictionary::database db, distributed<cql3::query_processor>& qp, sharded<service::storage_service>& ss, sharded<service::storage_proxy>& proxy,
            auth::service& auth_service, ::timeout_config timeout_config, service_permit& current_permit)
        : _db(db)
        , _query_processor(qp)
        , _ss(ss)
        , _proxy(proxy)
        , _timeout_config(timeout_config)
        , _client_state(service::client_state::external_tag{}, auth_service, nullptr, _timeout_config, socket_address(), true)
        // FIXME: Handlers are not created per query, but rather per connection, so it makes little sense to store
        // service permits in here. The query state should be reinstantiated per query - AFAIK it's only used
        // for CQL queries which piggy-back on Thrift protocol.
        , _query_state(_client_state, /*FIXME: pass real permit*/empty_service_permit())
        , _current_permit(current_permit)
    { }

    const sstring& current_keyspace() const {
        return _query_state.get_client_state().get_raw_keyspace();
    }

    void validate_login() const {
        return _query_state.get_client_state().validate_login();
    };

    void login(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const AuthenticationRequest& auth_request) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            auth::authenticator::credentials_map creds(auth_request.credentials.begin(), auth_request.credentials.end());
            auto& auth_service = *_query_state.get_client_state().get_auth_service();
            return auth_service.underlying_authenticator().authenticate(creds).then([this] (auto user) {
                _query_state.get_client_state().set_login(std::move(user));
            });
        });
    }

    void set_keyspace(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& keyspace) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            _query_state.get_client_state().set_keyspace(_db.real_database(), keyspace);
        });
    }

    void get(thrift_fn::function<void(ColumnOrSuperColumn const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnPath& column_path, const ConsistencyLevel::type consistency_level) {
        return get_slice([cob = std::move(cob)](auto&& results) {
            if (results.empty()) {
                throw NotFoundException();
            }
            return cob(std::move(results.front()));
        }, exn_cob, key, column_path_to_column_parent(column_path), column_path_to_slice_predicate(column_path), std::move(consistency_level));
    }

    void get_slice(thrift_fn::function<void(std::vector<ColumnOrSuperColumn>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnParent& column_parent, const SlicePredicate& predicate, const ConsistencyLevel::type consistency_level) {
        return multiget_slice([cob = std::move(cob)](auto&& results) {
            if (!results.empty()) {
                return cob(std::move(results.begin()->second));
            }
            return cob({ });
        }, exn_cob, {key}, column_parent, predicate, consistency_level);
    }

    void get_count(thrift_fn::function<void(int32_t const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnParent& column_parent, const SlicePredicate& predicate, const ConsistencyLevel::type consistency_level) {
        return multiget_count([cob = std::move(cob)](auto&& results) {
            if (!results.empty()) {
                return cob(results.begin()->second);
            }
            return cob(0);
        }, exn_cob, {key}, column_parent, predicate, consistency_level);
    }

    void multiget_slice(thrift_fn::function<void(std::map<std::string, std::vector<ColumnOrSuperColumn> >  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::vector<std::string> & keys, const ColumnParent& column_parent, const SlicePredicate& predicate, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_parent.column_family, [&](schema_ptr schema) {
            if (!column_parent.super_column.empty()) {
                fail(unimplemented::cause::SUPER);
            }
            auto& proxy = _proxy.local();
            auto cmd = slice_pred_to_read_cmd(proxy, *schema, predicate);
            auto cell_limit = predicate.__isset.slice_range ? static_cast<uint32_t>(predicate.slice_range.count) : std::numeric_limits<uint32_t>::max();
            auto pranges = make_partition_ranges(*schema, keys);
            auto f = _query_state.get_client_state().has_schema_access(*schema, auth::permission::SELECT);
            return f.then([this, &proxy, schema, cmd, pranges = std::move(pranges), cell_limit, consistency_level, keys, permit = std::move(permit)]() mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.read_timeout;
                return proxy.query(schema, cmd, std::move(pranges), cl_from_thrift(consistency_level), {timeout, std::move(permit), _query_state.get_client_state()}).then(
                        [schema, cmd, cell_limit, keys = std::move(keys)](service::storage_proxy::coordinator_query_result qr) {
                    return query::result_view::do_with(*qr.query_result, [schema, cmd, cell_limit, keys = std::move(keys)](query::result_view v) mutable {
                        if (schema->is_counter()) {
                            counter_column_aggregator aggregator(*schema, cmd->slice, cell_limit, std::move(keys));
                            v.consume(cmd->slice, aggregator);
                            return aggregator.release();
                        }
                        column_aggregator<query_order::no> aggregator(*schema, cmd->slice, cell_limit, std::move(keys));
                        v.consume(cmd->slice, aggregator);
                        return aggregator.release();
                    });
                });
            });
        });
    }

    void multiget_count(thrift_fn::function<void(std::map<std::string, int32_t>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::vector<std::string> & keys, const ColumnParent& column_parent, const SlicePredicate& predicate, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_parent.column_family, [&](schema_ptr schema) {
            if (!column_parent.super_column.empty()) {
                fail(unimplemented::cause::SUPER);
            }
            auto& proxy = _proxy.local();
            auto cmd = slice_pred_to_read_cmd(proxy, *schema, predicate);
            auto cell_limit = predicate.__isset.slice_range ? static_cast<uint32_t>(predicate.slice_range.count) : std::numeric_limits<uint32_t>::max();
            auto pranges = make_partition_ranges(*schema, keys);
            auto f = _query_state.get_client_state().has_schema_access(*schema, auth::permission::SELECT);
            return f.then([this, &proxy, schema, cmd, pranges = std::move(pranges), cell_limit, consistency_level, keys, permit = std::move(permit)]() mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.read_timeout;
                return proxy.query(schema, cmd, std::move(pranges), cl_from_thrift(consistency_level), {timeout, std::move(permit), _query_state.get_client_state()}).then(
                        [schema, cmd, cell_limit, keys = std::move(keys)](service::storage_proxy::coordinator_query_result qr) {
                    return query::result_view::do_with(*qr.query_result, [schema, cmd, cell_limit, keys = std::move(keys)](query::result_view v) mutable {
                        column_counter counter(*schema, cmd->slice, cell_limit, std::move(keys));
                        v.consume(cmd->slice, counter);
                        return counter.release();
                    });
                });
            });
        });
    }

    /**
     * In origin, empty partitions are returned as part of the KeySlice, for which the key will be filled
     * in but the columns vector will be empty. Since in our case we don't return empty partitions, we
     * don't know which partition keys in the specified range we should return back to the client. So for
     * now our behavior differs from Origin.
     */
    void get_range_slices(thrift_fn::function<void(std::vector<KeySlice>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const ColumnParent& column_parent, const SlicePredicate& predicate, const KeyRange& range, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_parent.column_family, [&](schema_ptr schema) {
            if (!column_parent.super_column.empty()) {
                fail(unimplemented::cause::SUPER);
            }
            auto& proxy = _proxy.local();
            auto&& prange = make_partition_range(*schema, range);
            auto cmd = slice_pred_to_read_cmd(proxy, *schema, predicate);
            // KeyRange::count is the number of thrift rows to return, while
            // SlicePredicte::slice_range::count limits the number of thrift columns.
            if (schema->thrift().is_dynamic()) {
                // For dynamic CFs we must limit the number of partitions returned.
                cmd->partition_limit = range.count;
            } else {
                // For static CFs each thrift row maps to a CQL row.
                cmd->set_row_limit(static_cast<uint64_t>(range.count));
            }
            auto f = _query_state.get_client_state().has_schema_access(*schema, auth::permission::SELECT);
            return f.then([this, &proxy, schema, cmd, prange = std::move(prange), consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.range_read_timeout;
                return proxy.query(schema, cmd, std::move(prange), cl_from_thrift(consistency_level), {timeout, std::move(permit), _query_state.get_client_state()}).then(
                        [schema, cmd](service::storage_proxy::coordinator_query_result qr) {
                    return query::result_view::do_with(*qr.query_result, [schema, cmd](query::result_view v) {
                        return to_key_slices(*schema, cmd->slice, v, std::numeric_limits<uint32_t>::max());
                    });
                });
            });
        });
    }

    static lw_shared_ptr<query::read_command> make_paged_read_cmd(service::storage_proxy& proxy, const schema& s, uint32_t column_limit, const std::string* start_column, const dht::partition_range_vector& range) {
        auto opts = query_opts(s);
        std::vector<query::clustering_range> clustering_ranges;
        query::column_id_vector regular_columns;
        uint64_t row_limit;
        uint32_t partition_limit;
        std::unique_ptr<query::specific_ranges> specific_ranges = nullptr;
        // KeyRange::count is the number of thrift columns to return (unlike get_range_slices).
        if (s.thrift().is_dynamic()) {
            // For dynamic CFs we must limit the number of rows returned. We use the query::specific_ranges to constrain
            // the first partition, of which we are only interested in the columns after start_column.
            row_limit = static_cast<uint64_t>(column_limit);
            partition_limit = query::max_partitions;
            if (start_column) {
                auto sr = query::specific_ranges(*range[0].start()->value().key(), {make_clustering_range_and_validate(s, *start_column, std::string())});
                specific_ranges = std::make_unique<query::specific_ranges>(std::move(sr));
            }
            regular_columns.emplace_back(s.regular_begin()->id);
        } else {
            // For static CFs we must limit the number of columns returned. Since we don't implement a cell limit,
            // we ask for as many partitions as those that are capable of exhausting the limit and later filter out
            // any excess cells.
            row_limit = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
            partition_limit = (column_limit + s.regular_columns_count() - 1) / s.regular_columns_count();
            schema::const_iterator start_col = start_column
                                             ? s.regular_lower_bound(to_bytes(*start_column))
                                             : s.regular_begin();
            regular_columns = add_columns(start_col, s.regular_end(), false);
        }
        clustering_ranges.emplace_back(query::clustering_range::make_open_ended_both_sides());
        auto slice = query::partition_slice(std::move(clustering_ranges), { }, std::move(regular_columns), opts,
                std::move(specific_ranges));
        auto cmd = make_lw_shared<query::read_command>(s.id(), s.version(), std::move(slice), proxy.get_max_result_size(slice),
                query::tombstone_limit(proxy.get_tombstone_limit()), query::row_limit(row_limit), query::partition_limit(partition_limit));
        cmd->allow_limit = db::allow_per_partition_rate_limit::yes;
        return cmd;
    }

    static future<> do_get_paged_slice(
            sharded<service::storage_proxy>& proxy,
            schema_ptr schema,
            uint32_t column_limit,
            dht::partition_range_vector range,
            const std::string* start_column,
            db::consistency_level consistency_level,
            const ::timeout_config& timeout_config,
            std::vector<KeySlice>& output,
            service::query_state& qs,
            service_permit permit) {
        auto cmd = make_paged_read_cmd(proxy.local(), *schema, column_limit, start_column, range);
        std::optional<partition_key> start_key;
        auto end = range[0].end();
        if (start_column && !schema->thrift().is_dynamic()) {
            // For static CFs, we must first query for a specific key so as to consume the remainder
            // of columns in that partition.
            start_key = range[0].start()->value().key();
            range = {dht::partition_range::make_singular(std::move(range[0].start()->value()))};
        }
        auto range1 = range; // query() below accepts an rvalue, so need a copy to reuse later
        auto timeout = db::timeout_clock::now() + timeout_config.range_read_timeout;
        return proxy.local().query(schema, cmd, std::move(range), consistency_level, {timeout, std::move(permit), qs.get_client_state()}).then(
                [schema, cmd, column_limit](service::storage_proxy::coordinator_query_result qr) {
            return query::result_view::do_with(*qr.query_result, [schema, cmd, column_limit](query::result_view v) {
                return to_key_slices(*schema, cmd->slice, v, column_limit);
            });
        }).then([&proxy, schema, cmd, column_limit, range = std::move(range1), consistency_level, start_key = std::move(start_key), end = std::move(end), &timeout_config, &output, &qs, permit = std::move(permit)](auto&& slices) mutable {
            auto columns = std::accumulate(slices.begin(), slices.end(), 0u, [](auto&& acc, auto&& ks) {
                return acc + ks.columns.size();
            });
            std::move(slices.begin(), slices.end(), std::back_inserter(output));
            if (columns == 0 || columns == column_limit || (slices.size() < cmd->partition_limit && columns < cmd->get_row_limit())) {
                if (!output.empty() || !start_key) {
                    if (range.size() > 1 && columns < column_limit) {
                        range.erase(range.begin());
                        return do_get_paged_slice(proxy, std::move(schema), column_limit - columns, std::move(range), nullptr, consistency_level, timeout_config, output, qs, std::move(permit));
                    }
                    return make_ready_future();
                }
                // The single, first partition we queried was empty, so retry with no start column.
            } else {
                start_key = key_from_thrift(*schema, to_bytes_view(output.back().key));
            }
            auto start = dht::decorate_key(*schema, std::move(*start_key));
            range[0] = dht::partition_range(dht::partition_range::bound(std::move(start), false), std::move(end));
            return do_get_paged_slice(proxy, schema, column_limit - columns, std::move(range), nullptr, consistency_level, timeout_config, output, qs, std::move(permit));
        });
    }

    void get_paged_slice(thrift_fn::function<void(std::vector<KeySlice> const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& column_family, const KeyRange& range, const std::string& start_column, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_family, [&](schema_ptr schema) {
            return do_with(std::vector<KeySlice>(), [&](auto& output) {
                if (range.__isset.row_filter) {
                    throw make_exception<InvalidRequestException>("Cross-row paging is not supported along with index clauses");
                }
                if (range.count <= 0) {
                    throw make_exception<InvalidRequestException>("Count must be positive");
                }
                auto&& prange = make_partition_range(*schema, range);
                if (!start_column.empty()) {
                    auto&& start_bound = prange[0].start();
                    if (!(start_bound && start_bound->is_inclusive() && start_bound->value().has_key())) {
                        // According to Orign's DataRange#Paging#slicesForKey.
                        throw make_exception<InvalidRequestException>("If start column is provided, so must the start key");
                    }
                }
                auto f = _query_state.get_client_state().has_schema_access(*schema, auth::permission::SELECT);
                return f.then([this, schema, count = range.count, start_column, prange = std::move(prange), consistency_level, &output, permit = std::move(permit)] () mutable {
                    return do_get_paged_slice(_proxy, std::move(schema), count, std::move(prange), &start_column,
                            cl_from_thrift(consistency_level), _timeout_config, output, _query_state, std::move(permit)).then([&output] {
                        return std::move(output);
                    });
                });
            });
        });
    }

    void get_indexed_slices(thrift_fn::function<void(std::vector<KeySlice>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const ColumnParent& column_parent, const IndexClause& index_clause, const SlicePredicate& column_predicate, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        std::vector<KeySlice>  _return;
        warn(unimplemented::cause::INDEXES);
        // FIXME: implement
        return pass_unimplemented(exn_cob);
    }

    void insert(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnParent& column_parent, const Column& column, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_parent.column_family, [&](schema_ptr schema) {
            if (column_parent.__isset.super_column) {
                fail(unimplemented::cause::SUPER);
            }

            if (schema->is_view()) {
                throw make_exception<InvalidRequestException>("Cannot modify Materialized Views directly");
            }

            mutation m_to_apply(schema, key_from_thrift(*schema, to_bytes_view(key)));
            add_to_mutation(*schema, column, m_to_apply);
            return _query_state.get_client_state().has_schema_access(*schema, auth::permission::MODIFY).then([this, m_to_apply = std::move(m_to_apply), consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.write_timeout;
                return _proxy.local().mutate({std::move(m_to_apply)}, cl_from_thrift(consistency_level), timeout, nullptr, std::move(permit), db::allow_per_partition_rate_limit::yes);
            });
        });
    }

    void add(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnParent& column_parent, const CounterColumn& column, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_parent.column_family, [&](schema_ptr schema) {
            if (column_parent.__isset.super_column) {
                fail(unimplemented::cause::SUPER);
            }

            mutation m_to_apply(schema, key_from_thrift(*schema, to_bytes_view(key)));
            add_to_mutation(*schema, column, m_to_apply);
            return _query_state.get_client_state().has_schema_access(*schema, auth::permission::MODIFY).then([this, m_to_apply = std::move(m_to_apply), consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.write_timeout;
                return _proxy.local().mutate({std::move(m_to_apply)}, cl_from_thrift(consistency_level), timeout, nullptr, std::move(permit), db::allow_per_partition_rate_limit::yes);
            });
        });
    }

    void cas(thrift_fn::function<void(CASResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const std::string& column_family, const std::vector<Column> & expected, const std::vector<Column> & updates, const ConsistencyLevel::type serial_consistency_level, const ConsistencyLevel::type commit_consistency_level) {
        service_permit permit = obtain_permit();
        CASResult _return;
        warn(unimplemented::cause::LWT);
        // FIXME: implement
        return pass_unimplemented(exn_cob);
    }

    void remove(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnPath& column_path, const int64_t timestamp, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_path.column_family, [&](schema_ptr schema) {
            if (schema->is_view()) {
                throw make_exception<InvalidRequestException>("Cannot modify Materialized Views directly");
            }

            mutation m_to_apply(schema, key_from_thrift(*schema, to_bytes_view(key)));

            if (column_path.__isset.super_column) {
                fail(unimplemented::cause::SUPER);
            } else if (column_path.__isset.column) {
                Deletion d;
                d.__set_timestamp(timestamp);
                d.__set_predicate(column_path_to_slice_predicate(column_path));
                Mutation m;
                m.__set_deletion(d);
                add_to_mutation(*schema, m, m_to_apply);
            } else {
                m_to_apply.partition().apply(tombstone(timestamp, gc_clock::now()));
            }

            return _query_state.get_client_state().has_schema_access(*schema, auth::permission::MODIFY).then([this, m_to_apply = std::move(m_to_apply), consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.write_timeout;
                return _proxy.local().mutate({std::move(m_to_apply)}, cl_from_thrift(consistency_level), timeout, nullptr, std::move(permit), db::allow_per_partition_rate_limit::yes);
            });
        });
    }

    void remove_counter(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& key, const ColumnPath& column_path, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), column_path.column_family, [&](schema_ptr schema) {
            mutation m_to_apply(schema, key_from_thrift(*schema, to_bytes_view(key)));

            auto timestamp = api::new_timestamp();
            if (column_path.__isset.super_column) {
                fail(unimplemented::cause::SUPER);
            } else if (column_path.__isset.column) {
                Deletion d;
                d.__set_timestamp(timestamp);
                d.__set_predicate(column_path_to_slice_predicate(column_path));
                Mutation m;
                m.__set_deletion(d);
                add_to_mutation(*schema, m, m_to_apply);
            } else {
                m_to_apply.partition().apply(tombstone(timestamp, gc_clock::now()));
            }

            return _query_state.get_client_state().has_schema_access(*schema, auth::permission::MODIFY).then([this, m_to_apply = std::move(m_to_apply), consistency_level, permit = std::move(permit)] () mutable {
                // This mutation contains only counter tombstones so it can be applied like non-counter mutations.
                auto timeout = db::timeout_clock::now() + _timeout_config.counter_write_timeout;
                return _proxy.local().mutate({std::move(m_to_apply)}, cl_from_thrift(consistency_level), timeout, nullptr, std::move(permit), db::allow_per_partition_rate_limit::yes);
            });
        });
    }

    void batch_mutate(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::map<std::string, std::map<std::string, std::vector<Mutation> > > & mutation_map, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            auto p = prepare_mutations(_db, current_keyspace(), mutation_map);
            return parallel_for_each(std::move(p.second), [this](auto&& schema) {
                return _query_state.get_client_state().has_schema_access(*schema, auth::permission::MODIFY);
            }).then([this, muts = std::move(p.first), consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.write_timeout;
                return _proxy.local().mutate(std::move(muts), cl_from_thrift(consistency_level), timeout, nullptr, std::move(permit), db::allow_per_partition_rate_limit::yes);
            });
        });
    }

    void atomic_batch_mutate(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::map<std::string, std::map<std::string, std::vector<Mutation> > > & mutation_map, const ConsistencyLevel::type consistency_level) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            auto p = prepare_mutations(_db, current_keyspace(), mutation_map);
            return parallel_for_each(std::move(p.second), [this](auto&& schema) {
                return _query_state.get_client_state().has_schema_access(*schema, auth::permission::MODIFY);
            }).then([this, muts = std::move(p.first), consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.write_timeout;
                return _proxy.local().mutate_atomically(std::move(muts), cl_from_thrift(consistency_level), timeout, nullptr, std::move(permit));
            });
        });
    }

    void truncate(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& cfname) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            if (current_keyspace().empty()) {
                throw make_exception<InvalidRequestException>("keyspace not set");
            }

            return _query_state.get_client_state().has_column_family_access(current_keyspace(), cfname, auth::permission::MODIFY).then([this, cfname] {
                if (_db.find_schema(current_keyspace(), cfname)->is_view()) {
                    throw make_exception<InvalidRequestException>("Cannot truncate Materialized Views");
                }
                return _proxy.local().truncate_blocking(current_keyspace(), cfname);
            });
        });
    }

    void get_multi_slice(thrift_fn::function<void(std::vector<ColumnOrSuperColumn>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const MultiSliceRequest& request) {
        service_permit permit = obtain_permit();
        with_schema(std::move(cob), std::move(exn_cob), request.column_parent.column_family, [&](schema_ptr schema) {
            if (!request.__isset.key) {
                throw make_exception<InvalidRequestException>("Key may not be empty");
            }
            if (!request.__isset.column_parent || request.column_parent.column_family.empty()) {
                throw make_exception<InvalidRequestException>("non-empty table is required");
            }
            if (!request.column_parent.super_column.empty()) {
                throw make_exception<InvalidRequestException>("get_multi_slice does not support super columns");
            }
            auto& s = *schema;
            auto pk = key_from_thrift(s, to_bytes(request.key));
            auto dk = dht::decorate_key(s, pk);
            query::column_id_vector regular_columns;
            std::vector<query::clustering_range> clustering_ranges;
            auto opts = query_opts(s);
            uint64_t row_limit;
            if (s.thrift().is_dynamic()) {
                row_limit = request.count;
                auto cmp = bound_view::compare(s);
                clustering_ranges = make_non_overlapping_ranges<clustering_key_prefix>(std::move(request.column_slices), [&s](auto&& cslice) {
                    return make_clustering_range(s, cslice.start, cslice.finish);
                }, clustering_key_prefix::prefix_equal_tri_compare(s), [cmp = std::move(cmp)](auto& range) {
                    auto bounds = bound_view::from_range(range);
                    return cmp(bounds.second, bounds.first);
                }, request.reversed);
                regular_columns.emplace_back(s.regular_begin()->id);
                if (request.reversed) {
                    opts.set(query::partition_slice::option::reversed);
                }
            } else {
                row_limit = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
                clustering_ranges.emplace_back(query::clustering_range::make_open_ended_both_sides());
                auto cmp = [&s](auto&& s1, auto&& s2) { return s.regular_column_name_type()->compare(s1, s2); };
                auto ranges = make_non_overlapping_ranges<bytes>(std::move(request.column_slices), [](auto&& cslice) {
                    return make_range(cslice.start, cslice.finish);
                }, cmp, [&](auto& range) { return range.is_wrap_around(cmp); }, request.reversed);
                auto on_range = [&](auto&& range) {
                    auto start = range.start() ? s.regular_lower_bound(range.start()->value()) : s.regular_begin();
                    auto end  = range.end() ? s.regular_upper_bound(range.end()->value()) : s.regular_end();
                    regular_columns = add_columns(start, end, request.reversed);
                };
                if (request.reversed) {
                    std::for_each(ranges.rbegin(), ranges.rend(), on_range);
                } else {
                    std::for_each(ranges.begin(), ranges.end(), on_range);
                }
            }
            auto slice = query::partition_slice(std::move(clustering_ranges), {}, std::move(regular_columns), opts, nullptr);
            auto& proxy = _proxy.local();
            auto cmd = make_lw_shared<query::read_command>(schema->id(), schema->version(), std::move(slice), proxy.get_max_result_size(slice),
                    query::tombstone_limit(proxy.get_tombstone_limit()), query::row_limit(row_limit));
            cmd->allow_limit = db::allow_per_partition_rate_limit::yes;
            auto f = _query_state.get_client_state().has_schema_access(*schema, auth::permission::SELECT);
            return f.then([this, &proxy, dk = std::move(dk), cmd, schema, column_limit = request.count, cl = request.consistency_level, permit = std::move(permit)] () mutable {
                auto timeout = db::timeout_clock::now() + _timeout_config.read_timeout;
                return proxy.query(schema, cmd, {dht::partition_range::make_singular(dk)}, cl_from_thrift(cl), {timeout, std::move(permit), _query_state.get_client_state()}).then(
                        [schema, cmd, column_limit](service::storage_proxy::coordinator_query_result qr) {
                    return query::result_view::do_with(*qr.query_result, [schema, cmd, column_limit](query::result_view v) {
                        column_aggregator<query_order::no> aggregator(*schema, cmd->slice, column_limit, { });
                        v.consume(cmd->slice, aggregator);
                        auto cols = aggregator.release();
                        return !cols.empty() ? std::move(cols.begin()->second) : std::vector<ColumnOrSuperColumn>();
                    });
                });
            });
        });
    }

    void describe_schema_versions(thrift_fn::function<void(std::map<std::string, std::vector<std::string> >  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this] {
            return _ss.local().describe_schema_versions().then([](auto&& m) {
                std::map<std::string, std::vector<std::string>> ret;
                for (auto&& p : m) {
                    ret[p.first] = std::vector<std::string>(p.second.begin(), p.second.end());
                }
                return ret;
            });
        });
    }

    void describe_keyspaces(thrift_fn::function<void(std::vector<KsDef>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            validate_login();
            std::vector<KsDef>  ret;
            for (auto&& ks : _db.get_keyspaces()) {
                ret.emplace_back(get_keyspace_definition(ks));
            }
            return ret;
        });
    }

    void describe_cluster_name(thrift_fn::function<void(std::string const& _return)> cob) {
        service_permit permit = obtain_permit();
        cob(_db.get_config().cluster_name());
    }

    void describe_version(thrift_fn::function<void(std::string const& _return)> cob) {
        service_permit permit = obtain_permit();
        cob(::cassandra::thrift_version);
    }

    void do_describe_ring(thrift_fn::function<void(std::vector<TokenRange>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& keyspace, bool local) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] () -> future<std::vector<TokenRange>> {
            auto ks = _db.find_keyspace(keyspace);
            if (ks.get_replication_strategy().get_type() == locator::replication_strategy_type::local) {
                throw make_exception<InvalidRequestException>("There is no ring for the keyspace: {}", keyspace);
            }

            auto ring = co_await _ss.local().describe_ring(keyspace, local);
            std::vector<TokenRange> ret;
            ret.reserve(ring.size());
            std::transform(ring.begin(), ring.end(), std::back_inserter(ret), [](auto&& tr) {
                TokenRange token_range;
                token_range.__set_start_token(std::move(tr._start_token));
                token_range.__set_end_token(std::move(tr._end_token));
                token_range.__set_endpoints(std::vector<std::string>(tr._endpoints.begin(), tr._endpoints.end()));
                std::vector<EndpointDetails> eds;
                std::transform(tr._endpoint_details.begin(), tr._endpoint_details.end(), std::back_inserter(eds), [](auto&& ed) {
                    EndpointDetails detail;
                    detail.__set_host(fmt::to_string(ed._host));
                    detail.__set_datacenter(ed._datacenter);
                    detail.__set_rack(ed._rack);
                    return detail;
                });
                token_range.__set_endpoint_details(std::move(eds));
                token_range.__set_rpc_endpoints(std::vector<std::string>(tr._rpc_endpoints.begin(), tr._rpc_endpoints.end()));
                return token_range;
            });
            co_return ret;
        });
    }

    void describe_ring(thrift_fn::function<void(std::vector<TokenRange>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& keyspace) {
        do_describe_ring(std::move(cob), std::move(exn_cob), keyspace, false);
    }

    void describe_local_ring(thrift_fn::function<void(std::vector<TokenRange>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& keyspace) {
        do_describe_ring(std::move(cob), std::move(exn_cob), keyspace, true);
    }

    void describe_token_map(thrift_fn::function<void(std::map<std::string, std::string>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this] {
            auto m = _ss.local().get_token_to_endpoint_map();
            std::map<std::string, std::string> ret;
            for (auto&& p : m) {
                ret[format("{}", p.first)] = p.second.to_sstring();
            }
            return ret;
        });
    }

    void describe_partitioner(thrift_fn::function<void(std::string const& _return)> cob) {
        service_permit permit = obtain_permit();
        cob(_db.get_config().partitioner());
    }

    void describe_snitch(thrift_fn::function<void(std::string const& _return)> cob) {
        service_permit permit = obtain_permit();
        cob(format("org.apache.cassandra.locator.{}", _db.real_database().get_snitch_name()));
    }

    void describe_keyspace(thrift_fn::function<void(KsDef const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& keyspace) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&] {
            validate_login();
            auto ks = _db.find_keyspace(keyspace);
            return get_keyspace_definition(ks);
        });
    }

    void describe_splits(thrift_fn::function<void(std::vector<std::string>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& cfName, const std::string& start_token, const std::string& end_token, const int32_t keys_per_split) {
        service_permit permit = obtain_permit();
        return describe_splits_ex([cob = std::move(cob)](auto&& results) {
            std::vector<std::string> res;
            res.reserve(results.size() + 1);
            res.emplace_back(results[0].start_token);
            for (auto&& s : results) {
                res.emplace_back(std::move(s.end_token));
            }
            return cob(std::move(res));
        }, exn_cob, cfName, start_token, end_token, keys_per_split);
    }

    void trace_next_query(thrift_fn::function<void(std::string const& _return)> cob) {
        service_permit permit = obtain_permit();
        std::string _return;
        // FIXME: implement
        return cob("dummy trace");
    }

    void describe_splits_ex(thrift_fn::function<void(std::vector<CfSplit>  const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& cfName, const std::string& start_token, const std::string& end_token, const int32_t keys_per_split) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [&]{
            dht::token_range_vector ranges;
            auto tstart = start_token.empty() ? dht::minimum_token() : dht::token::from_sstring(sstring(start_token));
            auto tend = end_token.empty() ? dht::maximum_token() : dht::token::from_sstring(sstring(end_token));
            range<dht::token> r({{ std::move(tstart), false }}, {{ std::move(tend), true }});
            auto cf = sstring(cfName);
            auto splits = _ss.local().get_splits(current_keyspace(), cf, std::move(r), keys_per_split);

            std::vector<CfSplit> res;
            for (auto&& s : splits) {
                res.emplace_back();
                assert(s.first.start() && s.first.end());
                auto start_token = s.first.start()->value().to_sstring();
                auto end_token = s.first.end()->value().to_sstring();
                res.back().__set_start_token(bytes_to_string(to_bytes_view(start_token)));
                res.back().__set_end_token(bytes_to_string(to_bytes_view(end_token)));
                res.back().__set_row_count(s.second);
            }
            return res;
        });
    }

    future<std::string> execute_schema_command(std::function<future<std::vector<mutation>>(data_dictionary::database, api::timestamp_type)> ddl, std::string_view description) {
        return _query_processor.invoke_on(0, [ddl = std::move(ddl), description = std::move(description)] (cql3::query_processor& qp) mutable {
            return qp.execute_thrift_schema_command(std::move(ddl), std::move(description));
        });
    }

    void system_add_column_family(thrift_fn::function<void(std::string const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const CfDef& cf_def) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this, def = cf_def] () -> future<std::string> {
            auto& t = *this;
            auto cf_def = def;

            co_await t._query_state.get_client_state().has_keyspace_access(cf_def.keyspace, auth::permission::CREATE);

            co_return co_await t.execute_schema_command([&p = t._proxy.local(), &cf_def] (data_dictionary::database db, api::timestamp_type ts) -> future<std::vector<mutation>> {
                if (!db.has_keyspace(cf_def.keyspace)) {
                    throw NotFoundException();
                }
                if (db.has_schema(cf_def.keyspace, cf_def.name)) {
                    throw make_exception<InvalidRequestException>("Column family {} already exists", cf_def.name);
                }

                auto s = schema_from_thrift(cf_def, cf_def.keyspace);
                co_return co_await service::prepare_new_column_family_announcement(p, std::move(s), ts);
            }, format("thrift: create column family {}", cf_def.name));
        });
    }
    void system_drop_column_family(thrift_fn::function<void(std::string const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& column_family) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this, cfm = column_family] () -> future<std::string> {
            auto& t = *this;
            auto column_family = cfm;
            co_await t._query_state.get_client_state().has_column_family_access(t.current_keyspace(), column_family, auth::permission::DROP);

            co_return co_await t.execute_schema_command(
                       [&p = t._proxy.local(), &column_family, &current_keyspace = t.current_keyspace()] (data_dictionary::database db, api::timestamp_type ts) -> future<std::vector<mutation>> {
                auto cf = db.find_table(current_keyspace, column_family);
                if (cf.schema()->is_view()) {
                    throw make_exception<InvalidRequestException>("Cannot drop Materialized Views from Thrift");
                }
                if (!cf.views().empty()) {
                    throw make_exception<InvalidRequestException>("Cannot drop table with Materialized Views {}", column_family);
                }

                co_return co_await service::prepare_column_family_drop_announcement(p, current_keyspace, column_family, ts);
            }, format("thrift: drop column family {}", column_family));
        });
    }

    void system_add_keyspace(thrift_fn::function<void(std::string const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const KsDef& ks_def) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this, def = ks_def] () -> future<std::string> {
            auto& t = *this;
            auto ks_def = def;

            co_await t._query_state.get_client_state().has_all_keyspaces_access(auth::permission::CREATE);

            co_return co_await t.execute_schema_command([&ks_def] (data_dictionary::database db, api::timestamp_type ts) -> future<std::vector<mutation>> {
                co_return service::prepare_new_keyspace_announcement(db.real_database(), keyspace_from_thrift(ks_def), ts);
            }, format("thrift: add {} keyspace", ks_def.name));
        });
    }

    void system_drop_keyspace(thrift_fn::function<void(std::string const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& keyspace) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this, ks = keyspace] () -> future<std::string> {
            auto& t = *this;
            auto keyspace = ks;

            co_await t._query_state.get_client_state().has_keyspace_access(keyspace, auth::permission::DROP);

            co_return co_await t.execute_schema_command([&keyspace] (data_dictionary::database db, api::timestamp_type ts) -> future<std::vector<mutation>> {
                thrift_validation::validate_keyspace_not_system(keyspace);
                if (!db.has_keyspace(keyspace)) {
                    throw NotFoundException();
                }

                co_return co_await service::prepare_keyspace_drop_announcement(db.real_database(), keyspace, ts);
            }, format("thrift: drop {} keyspace", keyspace));
        });
    }

    void system_update_keyspace(thrift_fn::function<void(std::string const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const KsDef& ks_def) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this, def = ks_def] () -> future<std::string> {
            auto& t = *this;
            auto ks_def = def;
            thrift_validation::validate_keyspace_not_system(ks_def.name);

            co_await t._query_state.get_client_state().has_keyspace_access(ks_def.name, auth::permission::ALTER);

            co_return co_await t.execute_schema_command([&ks_def] (data_dictionary::database db, api::timestamp_type ts) -> future<std::vector<mutation>> {
                if (!db.has_keyspace(ks_def.name)) {
                    throw NotFoundException();
                }
                if (!ks_def.cf_defs.empty()) {
                    throw make_exception<InvalidRequestException>("Keyspace update must not contain any column family definitions.");
                }

                auto ksm = keyspace_from_thrift(ks_def);
                co_return service::prepare_keyspace_update_announcement(db.real_database(), std::move(ksm), ts);
            }, format("thrift: update {} keyspace", ks_def.name));
        });
    }

    void system_update_column_family(thrift_fn::function<void(std::string const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const CfDef& cf_def) {
        service_permit permit = obtain_permit();
        with_cob(std::move(cob), std::move(exn_cob), [this, def = cf_def] () -> future<std::string> {
            auto& t = *this;
            auto cf_def = def;

            co_await t._query_state.get_client_state().has_schema_access(cf_def.keyspace, cf_def.name, auth::permission::ALTER);

            co_return co_await t.execute_schema_command([&p = t._proxy.local(), &cf_def] (data_dictionary::database db, api::timestamp_type ts) -> future<std::vector<mutation>> {
                auto cf = db.find_table(cf_def.keyspace, cf_def.name);
                auto schema = cf.schema();

                if (schema->is_cql3_table()) {
                    throw make_exception<InvalidRequestException>("Cannot modify CQL3 table {} as it may break the schema. You should use cqlsh to modify CQL3 tables instead.", cf_def.name);
                }

                if (schema->is_view()) {
                    throw make_exception<InvalidRequestException>("Cannot modify Materialized View table {} as it may break the schema. "
                                                                "You should use cqlsh to modify Materialized View tables instead.", cf_def.name);
                }

                if (!cf.views().empty()) {
                    throw make_exception<InvalidRequestException>("Cannot modify table with Materialized Views {} as it may break the schema. "
                                                                "You should use cqlsh to modify Materialized View tables instead.", cf_def.name);
                }

                auto s = schema_from_thrift(cf_def, cf_def.keyspace, schema->id());
                if (schema->thrift().is_dynamic() != s->thrift().is_dynamic()) {
                    fail(unimplemented::cause::MIXED_CF);
                }
                co_return co_await service::prepare_column_family_update_announcement(p, std::move(s), true, std::vector<view_ptr>(), ts);
            }, format("thrift: update column family {}", cf_def.name));
        });
    }

    void execute_cql_query(thrift_fn::function<void(CqlResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& query, const Compression::type compression) {
        throw make_exception<InvalidRequestException>("CQL2 is not supported");
    }

    class cql3_result_visitor final : public cql_transport::messages::result_message::visitor {
        CqlResult _result;
    public:
        const CqlResult& result() const {
            return _result;
        }
        virtual void visit(const cql_transport::messages::result_message::void_message&) override {
            _result.__set_type(CqlResultType::VOID);
        }
        virtual void visit(const cql_transport::messages::result_message::set_keyspace& m) override {
            _result.__set_type(CqlResultType::VOID);
        }
        virtual void visit(const cql_transport::messages::result_message::prepared::cql& m) override {
            throw make_exception<InvalidRequestException>("Cannot convert prepared query result to CqlResult");
        }
        virtual void visit(const cql_transport::messages::result_message::prepared::thrift& m) override {
            throw make_exception<InvalidRequestException>("Cannot convert prepared query result to CqlResult");
        }
        virtual void visit(const cql_transport::messages::result_message::schema_change& m) override {
            _result.__set_type(CqlResultType::VOID);
        }
        virtual void visit(const cql_transport::messages::result_message::rows& m) override {
            _result = to_thrift_result(m.rs());
        }
        virtual void visit(const cql_transport::messages::result_message::bounce_to_shard& m) override {
            throw TProtocolException(TProtocolException::TProtocolExceptionType::NOT_IMPLEMENTED, "Thrift does not support executing LWT statements");
        }
        virtual void visit(const cql_transport::messages::result_message::exception& m) override {
            m.throw_me();
        }
    };

    void execute_cql3_query(thrift_fn::function<void(CqlResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& query, const Compression::type compression, const ConsistencyLevel::type consistency) {
        with_exn_cob(std::move(exn_cob), [&] {
            if (compression != Compression::type::NONE) {
                throw make_exception<InvalidRequestException>("Compressed query strings are not supported");
            }
            auto& qp = _query_processor.local();
            auto opts = std::make_unique<cql3::query_options>(qp.get_cql_config(), cl_from_thrift(consistency), std::nullopt, std::vector<cql3::raw_value_view>(),
                            false, cql3::query_options::specific_options::DEFAULT);
            auto f = qp.execute_direct(query, _query_state, *opts);
            return f.then([cob = std::move(cob), opts = std::move(opts)](auto&& ret) {
                cql3_result_visitor visitor;
                ret->accept(visitor);
                return cob(visitor.result());
            });
        });
    }

    void prepare_cql_query(thrift_fn::function<void(CqlPreparedResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& query, const Compression::type compression) {
        throw make_exception<InvalidRequestException>("CQL2 is not supported");
    }

    class prepared_result_visitor final : public cql_transport::messages::result_message::visitor_base {
        CqlPreparedResult _result;
    public:
        const CqlPreparedResult& result() const {
            return _result;
        }
        virtual void visit(const cql_transport::messages::result_message::prepared::cql& m) override {
            throw std::runtime_error("Unexpected result message type.");
        }
        virtual void visit(const cql_transport::messages::result_message::prepared::thrift& m) override {
            _result.__set_itemId(m.get_id());
            auto& names = m.metadata().names();
            _result.__set_count(names.size());
            std::vector<std::string> variable_types;
            std::vector<std::string> variable_names;
            for (auto csp : names) {
                variable_types.emplace_back(csp->type->name());
                variable_names.emplace_back(csp->name->to_string());
            }
            _result.__set_variable_types(std::move(variable_types));
            _result.__set_variable_names(std::move(variable_names));
        }
    };

    void prepare_cql3_query(thrift_fn::function<void(CqlPreparedResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& query, const Compression::type compression) {
        with_exn_cob(std::move(exn_cob), [&] {
            validate_login();
            if (compression != Compression::type::NONE) {
                throw make_exception<InvalidRequestException>("Compressed query strings are not supported");
            }
            return _query_processor.local().prepare(query, _query_state).then([cob = std::move(cob)](auto&& stmt) {
                prepared_result_visitor visitor;
                stmt->accept(visitor);
                cob(visitor.result());
            });
        });
    }

    void execute_prepared_cql_query(thrift_fn::function<void(CqlResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const int32_t itemId, const std::vector<std::string> & values) {
        throw make_exception<InvalidRequestException>("CQL2 is not supported");
    }

    void execute_prepared_cql3_query(thrift_fn::function<void(CqlResult const& _return)> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const int32_t itemId, const std::vector<std::string> & values, const ConsistencyLevel::type consistency) {
        with_exn_cob(std::move(exn_cob), [&] {
            cql3::prepared_cache_key_type cache_key(itemId);
            bool needs_authorization = false;

            auto prepared = _query_processor.local().get_prepared(_query_state.get_client_state().user(), cache_key);
            if (!prepared) {
                needs_authorization = true;

                prepared = _query_processor.local().get_prepared(cache_key);
                if (!prepared) {
                    throw make_exception<InvalidRequestException>("Prepared query with id {} not found", itemId);
                }
            }
            auto stmt = prepared->statement;
            if (stmt->get_bound_terms() != values.size()) {
                throw make_exception<InvalidRequestException>("Wrong number of values specified. Expected {}, got {}.", stmt->get_bound_terms(), values.size());
            }
            std::vector<cql3::raw_value> bytes_values;
            std::transform(values.begin(), values.end(), std::back_inserter(bytes_values), [](auto&& s) {
                return cql3::raw_value::make_value(to_bytes(s));
            });
            auto& qp = _query_processor.local();
            auto opts = std::make_unique<cql3::query_options>(qp.get_cql_config(), cl_from_thrift(consistency), std::nullopt, std::move(bytes_values),
                            false, cql3::query_options::specific_options::DEFAULT);
            auto f = qp.execute_prepared(std::move(prepared), std::move(cache_key), _query_state, *opts, needs_authorization);
            return f.then([cob = std::move(cob), opts = std::move(opts)](auto&& ret) {
                cql3_result_visitor visitor;
                ret->accept(visitor);
                return cob(visitor.result());
            });
        });
    }

    void set_cql_version(thrift_fn::function<void()> cob, thrift_fn::function<void(::apache::thrift::TDelayedException* _throw)> exn_cob, const std::string& version) {
        // No-op.
        cob();
    }

private:
    template<allow_prefixes IsPrefixable>
    static sstring class_from_compound_type(const compound_type<IsPrefixable>& ct) {
        if (ct.is_singular()) {
            return ct.types().front()->name();
        }
        sstring type = "org.apache.cassandra.db.marshal.CompositeType(";
        for (auto& dt : ct.types()) {
            type += dt->name();
            if (&dt != &*ct.types().rbegin()) {
                type += ",";
            }
        }
        type += ")";
        return type;
    }
    static std::pair<std::vector<data_type>, bool> get_types(const std::string& thrift_type) {
        static const char composite_type[] = "CompositeType";
        std::vector<data_type> ret;
        auto t = sstring_view(thrift_type);
        auto composite_idx = t.find(composite_type);
        bool is_compound = false;
        if (composite_idx == sstring_view::npos) {
            ret.emplace_back(db::marshal::type_parser::parse(t));
        } else {
            t.remove_prefix(composite_idx + sizeof(composite_type) - 1);
            auto types = db::marshal::type_parser(t).get_type_parameters(false);
            std::move(types.begin(), types.end(), std::back_inserter(ret));
            is_compound = true;
        }
        return std::make_pair(std::move(ret), is_compound);
    }
    static CqlResult to_thrift_result(const cql3::result& rs) {
        CqlResult result;
        result.__set_type(CqlResultType::ROWS);

        constexpr static const char* utf8 = "UTF8Type";

        CqlMetadata mtd;
        std::map<std::string, std::string> name_types;
        std::map<std::string, std::string> value_types;
        for (auto&& c : rs.get_metadata().get_names()) {
            auto&& name = c->name->to_string();
            name_types.emplace(name, utf8);
            value_types.emplace(name, c->type->name());
        }
        mtd.__set_name_types(name_types);
        mtd.__set_value_types(value_types);
        mtd.__set_default_name_type(utf8);
        mtd.__set_default_value_type(utf8);
        result.__set_schema(mtd);

        struct visitor {
            std::vector<CqlRow> _rows;
            const cql3::metadata& _metadata;
            std::vector<Column> _columns;
            column_id _column_id;

            void start_row() {
                _column_id = 0;
                _columns.reserve(_metadata.column_count());
            }
            void accept_value(managed_bytes_view_opt cell) {
                auto& col = _metadata.get_names()[_column_id++];

                Column& c = _columns.emplace_back();
                c.__set_name(col->name->to_string());
                if (cell) {
                    c.__set_value(bytes_to_string(*cell));
                }

            }
            void end_row() {
                CqlRow& r = _rows.emplace_back();
                r.__set_key(std::string());
                r.__set_columns(std::move(_columns));
                _columns = { };
            }
        };

        visitor v { {}, rs.get_metadata(), {}, {} };
        rs.visit(v);
        result.__set_rows(std::move(v._rows));
        return result;
    }
    static KsDef get_keyspace_definition(const data_dictionary::keyspace& ks) {
        auto make_options = [](auto&& m) {
            return std::map<std::string, std::string>(m.begin(), m.end());
        };
        auto&& meta = ks.metadata();
        KsDef def;
        def.__set_name(meta->name());
        def.__set_strategy_class(meta->strategy_name());
        def.__set_strategy_options(make_options(meta->strategy_options()));
        std::vector<CfDef> cfs;
        for (auto&& s : meta->tables()) {
            if (s->is_cql3_table()) {
                continue;
            }
            CfDef cf_def;
            cf_def.__set_keyspace(s->ks_name());
            cf_def.__set_name(s->cf_name());
            cf_def.__set_column_type(cf_type_to_sstring(s->type()));
            cf_def.__set_comparator_type(cell_comparator::to_sstring(*s));
            cf_def.__set_comment(s->comment());
            cf_def.__set_read_repair_chance(s->read_repair_chance());
            std::vector<ColumnDef> columns;
            if (!s->thrift().is_dynamic()) {
                for (auto&& c : s->regular_columns()) {
                    ColumnDef c_def;
                    c_def.__set_name(c.name_as_text());
                    c_def.__set_validation_class(c.type->name());
                    columns.emplace_back(std::move(c_def));
                }
            }
            cf_def.__set_column_metadata(columns);
            cf_def.__set_gc_grace_seconds(s->gc_grace_seconds().count());
            cf_def.__set_default_validation_class(s->make_legacy_default_validator()->name());
            cf_def.__set_min_compaction_threshold(s->min_compaction_threshold());
            cf_def.__set_max_compaction_threshold(s->max_compaction_threshold());
            cf_def.__set_key_validation_class(class_from_compound_type(*s->partition_key_type()));
            cf_def.__set_key_alias(s->partition_key_columns().begin()->name_as_text());
            cf_def.__set_compaction_strategy(sstables::compaction_strategy::name(s->compaction_strategy()));
            cf_def.__set_compaction_strategy_options(make_options(s->compaction_strategy_options()));
            cf_def.__set_compression_options(make_options(s->get_compressor_params().get_options()));
            cf_def.__set_bloom_filter_fp_chance(s->bloom_filter_fp_chance());
            cf_def.__set_caching("all");
            cf_def.__set_dclocal_read_repair_chance(s->dc_local_read_repair_chance());
            cf_def.__set_memtable_flush_period_in_ms(s->memtable_flush_period());
            cf_def.__set_default_time_to_live(s->default_time_to_live().count());
            cf_def.__set_speculative_retry(s->speculative_retry().to_sstring());
            cfs.emplace_back(std::move(cf_def));
        }
        def.__set_cf_defs(cfs);
        def.__set_durable_writes(meta->durable_writes());
        return def;
    }
    static std::optional<index_metadata> index_metadata_from_thrift(const ColumnDef& def) {
        std::optional<sstring> idx_name;
        std::optional<std::unordered_map<sstring, sstring>> idx_opts;
        std::optional<index_metadata_kind> idx_type;
        if (def.__isset.index_type) {
            idx_type = [&def]() -> std::optional<index_metadata_kind> {
                switch (def.index_type) {
                    case IndexType::type::KEYS: return index_metadata_kind::keys;
                    case IndexType::type::COMPOSITES: return index_metadata_kind::composites;
                    case IndexType::type::CUSTOM: return index_metadata_kind::custom;
                    default: return {};
                };
            }();
        }
        if (def.__isset.index_name) {
            idx_name = to_sstring(def.index_name);
        }
        if (def.__isset.index_options) {
            idx_opts = std::unordered_map<sstring, sstring>(def.index_options.begin(), def.index_options.end());
        }
        if (idx_name && idx_opts && idx_type) {
            return index_metadata(idx_name.value(), idx_opts.value(), idx_type.value(), index_metadata::is_local_index::no);
        }
        return {};
    }
    static schema_ptr schema_from_thrift(const CfDef& cf_def, const sstring ks_name, std::optional<table_id> id = { }) {
        thrift_validation::validate_cf_def(cf_def);
        schema_builder builder(ks_name, cf_def.name, id);
        schema_builder::default_names names(builder);

        if (cf_def.__isset.key_validation_class) {
            auto pk_types = std::move(get_types(cf_def.key_validation_class).first);
            if (pk_types.size() == 1 && cf_def.__isset.key_alias) {
                builder.with_column(to_bytes(cf_def.key_alias), std::move(pk_types.back()), column_kind::partition_key);
            } else {
                for (uint32_t i = 0; i < pk_types.size(); ++i) {
                    builder.with_column(to_bytes(names.partition_key_name()), std::move(pk_types[i]), column_kind::partition_key);
                }
            }
        } else {
            builder.with_column(to_bytes(names.partition_key_name()), bytes_type, column_kind::partition_key);
        }

        auto default_validator = cf_def.__isset.default_validation_class
            ? db::marshal::type_parser::parse(to_sstring(cf_def.default_validation_class))
            : bytes_type;

        if (cf_def.column_metadata.empty()) {
            // Dynamic CF
            builder.set_is_dense(true);
            auto p = get_types(cf_def.comparator_type);
            auto ck_types = std::move(p.first);
            builder.set_is_compound(p.second);
            for (uint32_t i = 0; i < ck_types.size(); ++i) {
                builder.with_column(to_bytes(names.clustering_name()), std::move(ck_types[i]), column_kind::clustering_key);
            }
            builder.with_column(to_bytes(names.compact_value_name()), default_validator);
        } else {
            // Static CF
            builder.set_is_compound(false);
            auto column_name_type = db::marshal::type_parser::parse(to_sstring(cf_def.comparator_type));
            for (const ColumnDef& col_def : cf_def.column_metadata) {
                auto col_name = to_bytes(col_def.name);
                column_name_type->validate(col_name);
                builder.with_column(std::move(col_name), db::marshal::type_parser::parse(to_sstring(col_def.validation_class)),
                                    column_kind::regular_column);
                auto index = index_metadata_from_thrift(col_def);
                if (index) {
                    builder.with_index(index.value());
                }
            }
            builder.set_regular_column_name_type(column_name_type);
        }
        builder.set_default_validation_class(default_validator);
        if (cf_def.__isset.comment) {
            builder.set_comment(cf_def.comment);
        }
        if (cf_def.__isset.read_repair_chance) {
            builder.set_read_repair_chance(cf_def.read_repair_chance);
        }
        if (cf_def.__isset.gc_grace_seconds) {
            builder.set_gc_grace_seconds(cf_def.gc_grace_seconds);
        }
        if (cf_def.__isset.min_compaction_threshold) {
            builder.set_min_compaction_threshold(cf_def.min_compaction_threshold);
        }
        if (cf_def.__isset.max_compaction_threshold) {
            builder.set_max_compaction_threshold(cf_def.max_compaction_threshold);
        }
        if (cf_def.__isset.compaction_strategy) {
            builder.set_compaction_strategy(sstables::compaction_strategy::type(cf_def.compaction_strategy));
        }
        auto make_options = [](const std::map<std::string, std::string>& m) {
            return std::map<sstring, sstring>{m.begin(), m.end()};
        };
        if (cf_def.__isset.compaction_strategy_options) {
            builder.set_compaction_strategy_options(make_options(cf_def.compaction_strategy_options));
        }
        if (cf_def.__isset.compression_options) {
            builder.set_compressor_params(compression_parameters(make_options(cf_def.compression_options)));
        }
        if (cf_def.__isset.bloom_filter_fp_chance) {
            builder.set_bloom_filter_fp_chance(cf_def.bloom_filter_fp_chance);
        }
        if (cf_def.__isset.dclocal_read_repair_chance) {
            builder.set_dc_local_read_repair_chance(cf_def.dclocal_read_repair_chance);
        }
        if (cf_def.__isset.memtable_flush_period_in_ms) {
            builder.set_memtable_flush_period(cf_def.memtable_flush_period_in_ms);
        }
        if (cf_def.__isset.default_time_to_live) {
            builder.set_default_time_to_live(gc_clock::duration(cf_def.default_time_to_live));
        }
        if (cf_def.__isset.speculative_retry) {
            builder.set_speculative_retry(cf_def.speculative_retry);
        }
        if (cf_def.__isset.min_index_interval) {
            builder.set_min_index_interval(cf_def.min_index_interval);
        }
        if (cf_def.__isset.max_index_interval) {
            builder.set_max_index_interval(cf_def.max_index_interval);
        }
        return builder.build();
    }
    static lw_shared_ptr<keyspace_metadata> keyspace_from_thrift(const KsDef& ks_def) {
        thrift_validation::validate_ks_def(ks_def);
        std::vector<schema_ptr> cf_defs;
        cf_defs.reserve(ks_def.cf_defs.size());
        for (const CfDef& cf_def : ks_def.cf_defs) {
            if (cf_def.keyspace != ks_def.name) {
                throw make_exception<InvalidRequestException>("CfDef ({}) had a keyspace definition that did not match KsDef", cf_def.keyspace);
            }
            cf_defs.emplace_back(schema_from_thrift(cf_def, ks_def.name));
        }
        return make_lw_shared<keyspace_metadata>(
            ks_def.name,
            ks_def.strategy_class,
            std::map<sstring, sstring>{ks_def.strategy_options.begin(), ks_def.strategy_options.end()},
            std::nullopt,
            ks_def.durable_writes,
            std::move(cf_defs));
    }
    static schema_ptr lookup_schema(data_dictionary::database db, const sstring& ks_name, const sstring& cf_name) {
        if (ks_name.empty()) {
            throw make_exception<InvalidRequestException>("keyspace not set");
        }
        return db.find_schema(ks_name, cf_name);
    }
    static partition_key key_from_thrift(const schema& s, bytes_view k) {
        thrift_validation::validate_key(s, k);
        if (s.partition_key_size() == 1) {
            return partition_key::from_single_value(s, to_bytes(k));
        }
        auto composite = composite_view(k);
        return partition_key::from_exploded(composite.values());
    }
    static db::consistency_level cl_from_thrift(const ConsistencyLevel::type consistency_level) {
        switch (consistency_level) {
        case ConsistencyLevel::type::ONE: return db::consistency_level::ONE;
        case ConsistencyLevel::type::QUORUM: return db::consistency_level::QUORUM;
        case ConsistencyLevel::type::LOCAL_QUORUM: return db::consistency_level::LOCAL_QUORUM;
        case ConsistencyLevel::type::EACH_QUORUM: return db::consistency_level::EACH_QUORUM;
        case ConsistencyLevel::type::ALL: return db::consistency_level::ALL;
        case ConsistencyLevel::type::ANY: return db::consistency_level::ANY;
        case ConsistencyLevel::type::TWO: return db::consistency_level::TWO;
        case ConsistencyLevel::type::THREE: return db::consistency_level::THREE;
        case ConsistencyLevel::type::SERIAL: return db::consistency_level::SERIAL;
        case ConsistencyLevel::type::LOCAL_SERIAL: return db::consistency_level::LOCAL_SERIAL;
        case ConsistencyLevel::type::LOCAL_ONE: return db::consistency_level::LOCAL_ONE;
        default: throw make_exception<InvalidRequestException>("undefined consistency_level {}", consistency_level);
        }
    }
    static ttl_opt maybe_ttl(const schema& s, const Column& col) {
        if (col.__isset.ttl) {
            auto ttl = std::chrono::duration_cast<gc_clock::duration>(std::chrono::seconds(col.ttl));
            if (ttl.count() <= 0) {
                throw make_exception<InvalidRequestException>("ttl must be positive");
            }
            if (ttl > max_ttl) {
                throw make_exception<InvalidRequestException>("ttl is too large");
            }
            return {ttl};
        } else if (s.default_time_to_live().count() > 0) {
            return {s.default_time_to_live()};
        } else {
            return { };
        }
    }
    static void validate_key(const schema& s, const clustering_key& ck, bytes_view v) {
        auto ck_size = ck.size(s);
        if (ck_size > s.clustering_key_size()) {
            throw std::runtime_error(format("Cell name of {}.{} has too many components, expected {} but got {} in 0x{}",
                s.ks_name(), s.cf_name(), s.clustering_key_size(), ck_size, to_hex(v)));
        }
    }
    static clustering_key_prefix make_clustering_prefix(const schema& s, bytes_view v) {
        auto composite = composite_view(v, s.thrift().has_compound_comparator());
        auto ck = clustering_key_prefix::from_exploded(composite.values());
        validate_key(s, ck, v);
        return ck;
    }
    static query::clustering_range::bound make_clustering_bound(const schema& s, bytes_view v, composite::eoc exclusiveness_marker) {
        auto composite = composite_view(v, s.thrift().has_compound_comparator());
        auto last = composite::eoc::none;
        auto&& ck = clustering_key_prefix::from_exploded(composite.components() | boost::adaptors::transformed([&](auto&& c) {
            last = c.second;
            return c.first;
        }));
        validate_key(s, ck, v);
        return query::clustering_range::bound(std::move(ck), last != exclusiveness_marker);
    }
    static range<clustering_key_prefix> make_clustering_range(const schema& s, const std::string& start, const std::string& end) {
        using bound = range<clustering_key_prefix>::bound;
        std::optional<bound> start_bound;
        if (!start.empty()) {
            start_bound = make_clustering_bound(s, to_bytes_view(start), composite::eoc::end);
        }
        std::optional<bound> end_bound;
        if (!end.empty()) {
            end_bound = make_clustering_bound(s, to_bytes_view(end), composite::eoc::start);
        }
        return { std::move(start_bound), std::move(end_bound) };
    }
    static query::clustering_range make_clustering_range_and_validate(const schema& s, const std::string& start, const std::string& end) {
        auto range = make_clustering_range(s, start, end);
        auto bounds = bound_view::from_range(range);
        if (bound_view::compare(s)(bounds.second, bounds.first)) {
            throw make_exception<InvalidRequestException>("Range finish must come after start in the order of traversal");
        }
        return query::clustering_range(std::move(range));
    }
    static range<bytes> make_range(const std::string& start, const std::string& end) {
        using bound = range<bytes>::bound;
        std::optional<bound> start_bound;
        if (!start.empty()) {
            start_bound = bound(to_bytes(start));
        }
        std::optional<bound> end_bound;
        if (!end.empty()) {
            end_bound = bound(to_bytes(end));
        }
        return { std::move(start_bound), std::move(end_bound) };
    }
    static std::pair<schema::const_iterator, schema::const_iterator> make_column_range(const schema& s, const std::string& start, const std::string& end) {
        auto start_it = start.empty() ? s.regular_begin() : s.regular_lower_bound(to_bytes(start));
        auto end_it = end.empty() ? s.regular_end() : s.regular_upper_bound(to_bytes(end));
        if (start_it > end_it) {
            throw make_exception<InvalidRequestException>("Range finish must come after start in the order of traversal");
        }
        return std::make_pair(std::move(start_it), std::move(end_it));
    }
    // Adds the column_ids from the specified range of column_definitions to the out vector,
    // according to the order defined by reversed.
    template <typename Iterator>
    static query::column_id_vector add_columns(Iterator beg, Iterator end, bool reversed) {
        auto range = boost::make_iterator_range(std::move(beg), std::move(end))
                     | boost::adaptors::filtered(std::mem_fn(&column_definition::is_atomic))
                     | boost::adaptors::transformed(std::mem_fn(&column_definition::id));
        return reversed ? boost::copy_range<query::column_id_vector>(range | boost::adaptors::reversed)
                        : boost::copy_range<query::column_id_vector>(range);
    }
    static query::partition_slice::option_set query_opts(const schema& s) {
        query::partition_slice::option_set opts;
        if (!s.is_counter()) {
            opts.set(query::partition_slice::option::send_timestamp);
            opts.set(query::partition_slice::option::send_ttl);
        }
        if (s.thrift().is_dynamic()) {
            opts.set(query::partition_slice::option::send_clustering_key);
        }
        opts.set(query::partition_slice::option::send_partition_key);
        return opts;
    }
    static void sort_ranges(const schema& s, std::vector<query::clustering_range>& ranges) {
        position_in_partition::less_compare less(s);
        std::sort(ranges.begin(), ranges.end(),
            [&less] (const query::clustering_range& r1, const query::clustering_range& r2) {
                return less(
                    position_in_partition_view::for_range_start(r1),
                    position_in_partition_view::for_range_start(r2));
            });
    }
    static lw_shared_ptr<query::read_command> slice_pred_to_read_cmd(service::storage_proxy& proxy, const schema& s, const SlicePredicate& predicate) {
        auto opts = query_opts(s);
        std::vector<query::clustering_range> clustering_ranges;
        query::column_id_vector regular_columns;
        uint64_t per_partition_row_limit = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
        if (predicate.__isset.column_names) {
            thrift_validation::validate_column_names(predicate.column_names);
            auto unique_column_names = boost::copy_range<std::vector<std::string>>(predicate.column_names | boost::adaptors::uniqued);
            if (s.thrift().is_dynamic()) {
                for (auto&& name : unique_column_names) {
                    auto ckey = make_clustering_prefix(s, to_bytes(name));
                    clustering_ranges.emplace_back(query::clustering_range::make_singular(std::move(ckey)));
                }
                sort_ranges(s, clustering_ranges);
                regular_columns.emplace_back(s.regular_begin()->id);
            } else {
                clustering_ranges.emplace_back(query::clustering_range::make_open_ended_both_sides());
                auto&& defs = unique_column_names
                    | boost::adaptors::transformed([&s](auto&& name) { return s.get_column_definition(to_bytes(name)); })
                    | boost::adaptors::filtered([](auto* def) { return def; })
                    | boost::adaptors::indirected;
                regular_columns = add_columns(defs.begin(), defs.end(), false);
            }
        } else if (predicate.__isset.slice_range) {
            auto range = predicate.slice_range;
            if (range.count < 0) {
                throw make_exception<InvalidRequestException>("SliceRange requires non-negative count");
            }
            if (range.reversed) {
                std::swap(range.start, range.finish);
                opts.set(query::partition_slice::option::reversed);
            }
            per_partition_row_limit = static_cast<uint64_t>(range.count);
            if (s.thrift().is_dynamic()) {
                clustering_ranges.emplace_back(make_clustering_range_and_validate(s, range.start, range.finish));
                regular_columns.emplace_back(s.regular_begin()->id);
            } else {
                clustering_ranges.emplace_back(query::clustering_range::make_open_ended_both_sides());
                auto r = make_column_range(s, range.start, range.finish);
                // For static CFs, the limit is enforced on the result as we do not implement
                // a cell limit in the database engine.
                regular_columns = add_columns(r.first, r.second, range.reversed);
            }
        } else {
            throw make_exception<InvalidRequestException>("SlicePredicate column_names and slice_range may not both be null");
        }
        auto slice = query::partition_slice(std::move(clustering_ranges), {}, std::move(regular_columns), opts,
                nullptr, per_partition_row_limit);
        auto cmd = make_lw_shared<query::read_command>(s.id(), s.version(), std::move(slice), proxy.get_max_result_size(slice),
                query::tombstone_limit(proxy.get_tombstone_limit()));
        cmd->allow_limit = db::allow_per_partition_rate_limit::yes;
        return cmd;
    }
    static ColumnParent column_path_to_column_parent(const ColumnPath& column_path) {
        ColumnParent ret;
        ret.__set_column_family(column_path.column_family);
        if (column_path.__isset.super_column) {
            ret.__set_super_column(column_path.super_column);
        }
        return ret;
    }
    static SlicePredicate column_path_to_slice_predicate(const ColumnPath& column_path) {
        SlicePredicate ret;
        if (column_path.__isset.column) {
            ret.__set_column_names({column_path.column});
        }
        return ret;
    }
    static dht::partition_range_vector make_partition_ranges(const schema& s, const std::vector<std::string>& keys) {
        dht::partition_range_vector ranges;
        for (auto&& key : keys) {
            auto pk = key_from_thrift(s, to_bytes_view(key));
            auto dk = dht::decorate_key(s, pk);
            ranges.emplace_back(dht::partition_range::make_singular(std::move(dk)));
        }
        return ranges;
    }
    static Column make_column(const bytes& col, const query::result_atomic_cell_view& cell) {
        Column ret;
        ret.__set_name(bytes_to_string(col));
        ret.__set_value(bytes_to_string(cell.value()));
        ret.__set_timestamp(cell.timestamp());
        if (cell.ttl()) {
            ret.__set_ttl(cell.ttl()->count());
        }
        return ret;
    }
    static ColumnOrSuperColumn column_to_column_or_supercolumn(Column&& col) {
        ColumnOrSuperColumn ret;
        ret.__set_column(std::move(col));
        return ret;
    }
    static ColumnOrSuperColumn make_column_or_supercolumn(const bytes& col, const query::result_atomic_cell_view& cell) {
        return column_to_column_or_supercolumn(make_column(col, cell));
    }
    static CounterColumn make_counter_column(const bytes& col, const query::result_atomic_cell_view& cell) {
        CounterColumn ret;
        ret.__set_name(bytes_to_string(col));
        cell.value().with_linearized([&] (bytes_view value_view) {
            ret.__set_value(value_cast<int64_t>(long_type->deserialize_value(value_view)));
        });
        return ret;
    }
    static ColumnOrSuperColumn counter_column_to_column_or_supercolumn(CounterColumn&& col) {
        ColumnOrSuperColumn ret;
        ret.__set_counter_column(std::move(col));
        return ret;
    }
    static ColumnOrSuperColumn make_counter_column_or_supercolumn(const bytes& col, const query::result_atomic_cell_view& cell) {
        return counter_column_to_column_or_supercolumn(make_counter_column(col, cell));
    }
    static std::string partition_key_to_string(const schema& s, const partition_key& key) {
        return bytes_to_string(to_legacy(*s.partition_key_type(), key.representation()));
    }

    template<typename Aggregator, query_order QueryOrder>
    struct partition_index;

    template<typename Aggregator>
    struct partition_index<Aggregator, query_order::no> {
        using partition_type = std::map<std::string, typename Aggregator::type>;
        partition_type _aggregation;
        partition_index(std::vector<std::string>&& expected) {
            // For compatibility reasons, return expected keys even if they don't exist
            for (auto&& k : expected) {
                _aggregation[std::move(k)] = { };
            }
        }
        typename Aggregator::type* begin_aggregation(std::string partition_key) {
            return &_aggregation[std::move(partition_key)];
        }
    };
    template<typename Aggregator>
    struct partition_index<Aggregator, query_order::yes> {
        using partition_type = std::vector<std::pair<std::string, typename Aggregator::type>>;
        partition_type _aggregation;
        partition_index(std::vector<std::string>&& expected) {
        }
        typename Aggregator::type* begin_aggregation(std::string partition_key) {
            _aggregation.emplace_back(std::move(partition_key), typename Aggregator::type());
            return &_aggregation.back().second;
        }
    };

    template<typename Aggregator, query_order QueryOrder>
    requires thrift::Aggregator<Aggregator>
    class column_visitor : public Aggregator {
        const schema& _s;
        const query::partition_slice& _slice;
        const uint32_t _cell_limit;
        uint32_t _current_cell_limit;
        typename Aggregator::type* _current_aggregation;
        partition_index<Aggregator, QueryOrder> _index;
    public:
        column_visitor(const schema& s, const query::partition_slice& slice, uint32_t cell_limit, std::vector<std::string>&& expected)
                : _s(s), _slice(slice), _cell_limit(cell_limit), _current_cell_limit(0), _index(std::move(expected)) {
        }
        typename partition_index<Aggregator, QueryOrder>::partition_type&& release() {
            return std::move(_index._aggregation);
        }
        void accept_new_partition(const partition_key& key, uint32_t row_count) {
            _current_aggregation = _index.begin_aggregation(partition_key_to_string(_s, key));
            _current_cell_limit = _cell_limit;
        }
        void accept_new_partition(uint32_t row_count) {
            // We always ask for the partition_key to be sent in query_opts().
            abort();
        }
        void accept_new_row(const clustering_key_prefix& key, const query::result_row_view& static_row, const query::result_row_view& row) {
            auto it = row.iterator();
            auto cell = it.next_atomic_cell();
            if (cell && _current_cell_limit > 0) {
                bytes column_name = composite::serialize_value(key.components(), _s.thrift().has_compound_comparator()).release_bytes();
                Aggregator::on_column(_current_aggregation, column_name, *cell);
                _current_cell_limit -= 1;
            }
        }
        void accept_new_row(const query::result_row_view& static_row, const query::result_row_view& row) {
            auto it = row.iterator();
            for (auto&& id : _slice.regular_columns) {
                auto cell = it.next_atomic_cell();
                if (cell && _current_cell_limit > 0) {
                    Aggregator::on_column(_current_aggregation, _s.regular_column_at(id).name(), *cell);
                    _current_cell_limit -= 1;
                }
            }
        }
        void accept_partition_end(const query::result_row_view& static_row) {
        }
    };
    struct column_or_supercolumn_builder {
        using type = std::vector<ColumnOrSuperColumn>;
        void on_column(std::vector<ColumnOrSuperColumn>* current_cols, const bytes& name, const query::result_atomic_cell_view& cell) {
            current_cols->emplace_back(make_column_or_supercolumn(name, cell));
        }
    };
    template<query_order QueryOrder>
    using column_aggregator = column_visitor<column_or_supercolumn_builder, QueryOrder>;
    struct counter_column_or_supercolumn_builder {
        using type = std::vector<ColumnOrSuperColumn>;
        void on_column(std::vector<ColumnOrSuperColumn>* current_cols, const bytes& name, const query::result_atomic_cell_view& cell) {
            current_cols->emplace_back(make_counter_column_or_supercolumn(name, cell));
        }
    };
    using counter_column_aggregator = column_visitor<counter_column_or_supercolumn_builder, query_order::no>;
    struct counter {
        using type = int32_t;
        void on_column(int32_t* current_cols, const bytes_view& name, const query::result_atomic_cell_view& cell) {
            *current_cols += 1;
        }
    };
    using column_counter = column_visitor<counter, query_order::no>;
    static dht::partition_range_vector make_partition_range(const schema& s, const KeyRange& range) {
        if (range.__isset.row_filter) {
            fail(unimplemented::cause::INDEXES);
        }
        if ((range.__isset.start_key == range.__isset.start_token)
                || (range.__isset.end_key == range.__isset.end_token)) {
            throw make_exception<InvalidRequestException>(
                    "Exactly one each of {start key, start token} and {end key, end token} must be specified");
        }
        if (range.__isset.start_token && range.__isset.end_key) {
            throw make_exception<InvalidRequestException>("Start token + end key is not a supported key range");
        }

        auto&& partitioner = s.get_partitioner();

        if (range.__isset.start_key && range.__isset.end_key) {
            auto start = range.start_key.empty()
                       ? dht::ring_position::starting_at(dht::minimum_token())
                       : partitioner.decorate_key(s, key_from_thrift(s, to_bytes(range.start_key)));
            auto end = range.end_key.empty()
                     ? dht::ring_position::ending_at(dht::maximum_token())
                     : partitioner.decorate_key(s, key_from_thrift(s, to_bytes(range.end_key)));
            if (end.less_compare(s, start)) {
                throw make_exception<InvalidRequestException>(
                        "Start key's token sorts after end key's token. This is not allowed; you probably should not specify end key at all except with an ordered partitioner");
            }
            return {{dht::partition_range::bound(std::move(start), true),
                     dht::partition_range::bound(std::move(end), true)}};
        }

        if (range.__isset.start_key && range.__isset.end_token) {
            // start_token/end_token can wrap, but key/token should not
            auto start = range.start_key.empty()
                       ? dht::ring_position::starting_at(dht::minimum_token())
                       : partitioner.decorate_key(s, key_from_thrift(s, to_bytes(range.start_key)));
            auto end = dht::ring_position::ending_at(dht::token::from_sstring(sstring(range.end_token)));
            if (end.token().is_minimum()) {
                end = dht::ring_position::ending_at(dht::maximum_token());
            } else if (end.less_compare(s, start)) {
                throw make_exception<InvalidRequestException>("Start key's token sorts after end token");
            }
            return {{dht::partition_range::bound(std::move(start), true),
                     dht::partition_range::bound(std::move(end), true)}};
        }

        // Token range can wrap; the start token is exclusive.
        auto start = dht::ring_position::ending_at(dht::token::from_sstring(sstring(range.start_token)));
        auto end = dht::ring_position::ending_at(dht::token::from_sstring(sstring(range.end_token)));
        if (end.token().is_minimum()) {
            end = dht::ring_position::ending_at(dht::maximum_token());
        }
        // Special case of start == end also generates wrap-around range
        if (start.token() >= end.token()) {
            return {dht::partition_range(dht::partition_range::bound(std::move(start), false), {}),
                    dht::partition_range({}, dht::partition_range::bound(std::move(end), true))};
        }
        return {{dht::partition_range::bound(std::move(start), false),
                 dht::partition_range::bound(std::move(end), true)}};
    }
    static std::vector<KeySlice> to_key_slices(const schema& s, const query::partition_slice& slice, query::result_view v, uint32_t cell_limit) {
        column_aggregator<query_order::yes> aggregator(s, slice, cell_limit, { });
        v.consume(slice, aggregator);
        auto&& cols = aggregator.release();
        std::vector<KeySlice> ret;
        std::transform(
                std::make_move_iterator(cols.begin()),
                std::make_move_iterator(cols.end()),
                boost::back_move_inserter(ret),
                [](auto&& p) {
            KeySlice ks;
            ks.__set_key(std::move(p.first));
            ks.__set_columns(std::move(p.second));
            return ks;
        });
        return ret;
    }
    template<typename RangeType, typename Comparator, typename RangeComparator>
    static std::vector<nonwrapping_range<RangeType>> make_non_overlapping_ranges(
            std::vector<ColumnSlice> column_slices,
            const std::function<wrapping_range<RangeType>(ColumnSlice&&)> mapper,
            Comparator&& cmp,
            RangeComparator&& is_wrap_around,
            bool reversed) {
        std::vector<nonwrapping_range<RangeType>> ranges;
        std::transform(column_slices.begin(), column_slices.end(), std::back_inserter(ranges), [&](auto&& cslice) {
            auto range = mapper(std::move(cslice));
            if (!reversed && is_wrap_around(range)) {
                throw make_exception<InvalidRequestException>("Column slice had start {} greater than finish {}", cslice.start, cslice.finish);
            } else if (reversed && !is_wrap_around(range)) {
                throw make_exception<InvalidRequestException>("Reversed column slice had start {} less than finish {}", cslice.start, cslice.finish);
            } else if (reversed) {
                range.reverse();
                if (is_wrap_around(range)) {
                    // If a wrap around range is still wrapping after reverse, then it's (a, a). This is equivalent
                    // to an open ended range.
                    range = wrapping_range<RangeType>::make_open_ended_both_sides();
                }
            }
            return nonwrapping_range<RangeType>(std::move(range));
        });
        return nonwrapping_range<RangeType>::deoverlap(std::move(ranges), std::forward<Comparator>(cmp));
    }
    static range_tombstone make_range_tombstone(const schema& s, const SliceRange& range, tombstone tomb) {
        using bound = query::clustering_range::bound;
        std::optional<bound> start_bound;
        if (!range.start.empty()) {
            start_bound = make_clustering_bound(s, to_bytes_view(range.start), composite::eoc::end);
        }
        std::optional<bound> end_bound;
        if (!range.finish.empty()) {
            end_bound = make_clustering_bound(s, to_bytes_view(range.finish), composite::eoc::start);
        }
        return {start_bound ? std::move(*start_bound).value() : clustering_key_prefix::make_empty(),
                !start_bound || start_bound->is_inclusive() ? bound_kind::incl_start : bound_kind::excl_start,
                end_bound ? std::move(*end_bound).value() : clustering_key_prefix::make_empty(),
                !end_bound || end_bound->is_inclusive() ? bound_kind::incl_end : bound_kind::excl_end,
                std::move(tomb)};
    }
    static void delete_cell(const column_definition& def, api::timestamp_type timestamp, gc_clock::time_point deletion_time, mutation& m_to_apply) {
        if (def.is_atomic()) {
            auto dead_cell = atomic_cell::make_dead(timestamp, deletion_time);
            m_to_apply.set_clustered_cell(clustering_key_prefix::make_empty(), def, std::move(dead_cell));
        }
    }
    static void delete_column(const schema& s, const sstring& column_name, api::timestamp_type timestamp, gc_clock::time_point deletion_time, mutation& m_to_apply) {
        auto&& def = s.get_column_definition(to_bytes(column_name));
        if (def) {
            delete_cell(*def, timestamp, deletion_time, m_to_apply);
        }
    }
    static void apply_delete(const schema& s, const SlicePredicate& predicate, api::timestamp_type timestamp, mutation& m_to_apply) {
        auto deletion_time = gc_clock::now();
        if (predicate.__isset.column_names) {
            thrift_validation::validate_column_names(predicate.column_names);
            if (s.thrift().is_dynamic()) {
                for (auto&& name : predicate.column_names) {
                    auto ckey = make_clustering_prefix(s, to_bytes(name));
                    m_to_apply.partition().apply_delete(s, std::move(ckey), tombstone(timestamp, deletion_time));
                }
            } else {
                for (auto&& name : predicate.column_names) {
                    delete_column(s, name, timestamp, deletion_time, m_to_apply);
                }
            }
        } else if (predicate.__isset.slice_range) {
            auto&& range = predicate.slice_range;
            if (s.thrift().is_dynamic()) {
                m_to_apply.partition().apply_delete(s, make_range_tombstone(s, range, tombstone(timestamp, deletion_time)));
            } else {
                auto r = make_column_range(s, range.start, range.finish);
                std::for_each(r.first, r.second, [&](auto&& def) {
                    delete_cell(def, timestamp, deletion_time, m_to_apply);
                });
            }
        } else {
            throw make_exception<InvalidRequestException>("SlicePredicate column_names and slice_range may not both be null");
        }
    }
    static void add_live_cell(const schema& s, const Column& col, const column_definition& def, clustering_key_prefix ckey, mutation& m_to_apply) {
        thrift_validation::validate_column(col, def);
        auto cell = atomic_cell::make_live(*def.type, col.timestamp, to_bytes_view(col.value), maybe_ttl(s, col));
        m_to_apply.set_clustered_cell(std::move(ckey), def, std::move(cell));
    }
    static void add_live_cell(const schema& s, const CounterColumn& col, const column_definition& def, clustering_key_prefix ckey, mutation& m_to_apply) {
        //thrift_validation::validate_column(col, def);
        auto cell = atomic_cell::make_live_counter_update(api::new_timestamp(), col.value);
        m_to_apply.set_clustered_cell(std::move(ckey), def, std::move(cell));
    }
    static void add_to_mutation(const schema& s, const CounterColumn& col, mutation& m_to_apply) {
        thrift_validation::validate_column_name(col.name);
        if (s.thrift().is_dynamic()) {
            auto&& value_col = s.regular_begin();
            add_live_cell(s, col, *value_col, make_clustering_prefix(s, to_bytes_view(col.name)), m_to_apply);
        } else {
            auto def = s.get_column_definition(to_bytes(col.name));
            if (def) {
                if (def->kind != column_kind::regular_column) {
                    throw make_exception<InvalidRequestException>("Column {} is not settable", col.name);
                }
                add_live_cell(s, col, *def, clustering_key_prefix::make_empty(s), m_to_apply);
            } else {
                fail(unimplemented::cause::MIXED_CF);
            }
        }
    }
    static void add_to_mutation(const schema& s, const Column& col, mutation& m_to_apply) {
        thrift_validation::validate_column_name(col.name);
        if (s.thrift().is_dynamic()) {
            auto&& value_col = s.regular_begin();
            add_live_cell(s, col, *value_col, make_clustering_prefix(s, to_bytes_view(col.name)), m_to_apply);
        } else {
            auto def = s.get_column_definition(to_bytes(col.name));
            if (def) {
                if (def->kind != column_kind::regular_column) {
                    throw make_exception<InvalidRequestException>("Column {} is not settable", col.name);
                }
                add_live_cell(s, col, *def, clustering_key_prefix::make_empty(s), m_to_apply);
            } else {
                fail(unimplemented::cause::MIXED_CF);
            }
        }
    }
    static void add_to_mutation(const schema& s, const Mutation& m, mutation& m_to_apply) {
        if (m.__isset.column_or_supercolumn) {
            if (m.__isset.deletion) {
                throw make_exception<InvalidRequestException>("Mutation must have one and only one of column_or_supercolumn or deletion");
            }
            auto&& cosc = m.column_or_supercolumn;
            if (cosc.__isset.column + cosc.__isset.super_column + cosc.__isset.counter_column + cosc.__isset.counter_super_column != 1) {
                throw make_exception<InvalidRequestException>("ColumnOrSuperColumn must have one (and only one) of column, super_column, counter and counter_super_column");
            }
            if (cosc.__isset.column) {
                add_to_mutation(s, cosc.column, m_to_apply);
            } else if (cosc.__isset.super_column) {
                fail(unimplemented::cause::SUPER);
            } else if (cosc.__isset.counter_column) {
                add_to_mutation(s, cosc.counter_column, m_to_apply);
            } else if (cosc.__isset.counter_super_column) {
                fail(unimplemented::cause::SUPER);
            }
        } else if (m.__isset.deletion) {
            auto&& del = m.deletion;
            if (del.__isset.super_column) {
                fail(unimplemented::cause::SUPER);
            } else if (del.__isset.predicate) {
                apply_delete(s, del.predicate, del.timestamp, m_to_apply);
            } else {
                m_to_apply.partition().apply(tombstone(del.timestamp, gc_clock::now()));
            }
        } else {
            throw make_exception<InvalidRequestException>("Mutation must have either column or deletion");
        }
    }
    using mutation_map = std::map<std::string, std::map<std::string, std::vector<Mutation>>>;
    using mutation_map_by_cf = std::unordered_map<std::string, std::unordered_map<std::string, std::vector<Mutation>>>;
    static mutation_map_by_cf group_by_cf(mutation_map& m) {
        mutation_map_by_cf ret;
        for (auto&& key_cf : m) {
            for (auto&& cf_mutations : key_cf.second) {
                auto& mutations = ret[std::move(cf_mutations.first)][std::move(key_cf.first)];
                std::move(cf_mutations.second.begin(), cf_mutations.second.end(), std::back_inserter(mutations));
            }
        }
        return ret;
    }
    static std::pair<std::vector<mutation>, std::vector<schema_ptr>> prepare_mutations(data_dictionary::database db, const sstring& ks_name, const mutation_map& m) {
        std::vector<mutation> muts;
        std::vector<schema_ptr> schemas;
        auto m_by_cf = group_by_cf(const_cast<mutation_map&>(m));
        for (auto&& cf_key : m_by_cf) {
            auto schema = lookup_schema(db, ks_name, cf_key.first);
            if (schema->is_view()) {
                throw make_exception<InvalidRequestException>("Cannot modify Materialized Views directly");
            }
            schemas.emplace_back(schema);
            for (auto&& key_mutations : cf_key.second) {
                mutation m_to_apply(schema, key_from_thrift(*schema, to_bytes_view(key_mutations.first)));
                for (auto&& m : key_mutations.second) {
                    add_to_mutation(*schema, m, m_to_apply);
                }
                muts.emplace_back(std::move(m_to_apply));
            }
        }
        return {std::move(muts), std::move(schemas)};
    }
protected:
    service_permit obtain_permit() {
        return std::move(_current_permit);
    }
};

class handler_factory : public CassandraCobSvIfFactory {
    data_dictionary::database _db;
    distributed<cql3::query_processor>& _query_processor;
    sharded<service::storage_service>& _ss;
    sharded<service::storage_proxy>& _proxy;
    auth::service& _auth_service;
    const updateable_timeout_config& _timeout_config;
    service_permit& _current_permit;
public:
    explicit handler_factory(data_dictionary::database db,
                             distributed<cql3::query_processor>& qp,
                             sharded<service::storage_service>& ss,
                             sharded<service::storage_proxy>& proxy,
                             auth::service& auth_service,
                             const ::updateable_timeout_config& timeout_config,
                             service_permit& current_permit)
        : _db(db), _query_processor(qp), _ss(ss), _proxy(proxy), _auth_service(auth_service), _timeout_config(timeout_config), _current_permit(current_permit) {}
    typedef CassandraCobSvIf Handler;
    virtual CassandraCobSvIf* getHandler(const ::apache::thrift::TConnectionInfo& connInfo) {
        return new thrift_handler(_db, _query_processor, _ss, _proxy, _auth_service, _timeout_config.current_values(), _current_permit);
    }
    virtual void releaseHandler(CassandraCobSvIf* handler) {
        delete handler;
    }
};

std::unique_ptr<CassandraCobSvIfFactory>
create_handler_factory(data_dictionary::database db, distributed<cql3::query_processor>& qp,
        sharded<service::storage_service>& ss, sharded<service::storage_proxy>& proxy,
        auth::service& auth_service, const ::updateable_timeout_config& timeout_config, service_permit& current_permit) {
    return std::make_unique<handler_factory>(db, qp, ss, proxy, auth_service, timeout_config, current_permit);
}
