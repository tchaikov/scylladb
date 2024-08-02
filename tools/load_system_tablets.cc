/*
 * Copyright (C) 2024-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "tools/load_system_tablets.hh"

#include <seastar/core/thread.hh>
#include <seastar/util/closeable.hh>

#include "log.hh"
#include "partition_slice_builder.hh"
#include "data_dictionary/keyspace_metadata.hh"
#include "db/cql_type_parser.hh"
#include "db/system_keyspace.hh"
#include "mutation/mutation.hh"
#include "readers/combined.hh"
#include "reader_concurrency_semaphore.hh"
#include "tools/sstable_manager_service.hh"
#include "types/list.hh"
#include "types/tuple.hh"

namespace {

logging::logger logger{"load_sys_tablets"};


future<std::filesystem::path> get_table_directory(std::filesystem::path scylla_data_path,
                                                  std::string_view keyspace_name,
                                                  std::string_view table_name) {
    auto system_tables_path = scylla_data_path / table_name;
    auto system_tables_dir = co_await open_directory(system_tables_path.native());
    sstring found;
    // locate a directory named like
    //   "$scylla_data_path/system/tablets-fd4f7a4696bd3e7391bf99eb77e82a5c"
    auto h = system_tables_dir.list_directory([&] (directory_entry de) -> future<> {
        if (!found.empty()) {
            return make_ready_future();
        }

        auto dash_pos = de.name.find_last_of('-');
        if (dash_pos == de.name.npos) {
            // unlikely. but this should not be fatal
            return make_ready_future();
        }
        if (de.name.substr(0, dash_pos) != table_name) {
            return make_ready_future();
        }
        if (!de.type) {
            throw std::runtime_error(fmt::format("failed to load system.tablets from {}/{}: unrecognized type", scylla_data_path, de.name));
        }
        if (*de.type != directory_entry_type::directory) {
            throw std::runtime_error(fmt::format("failed to load system.tablets from {}/{}: not a directory", scylla_data_path, de.name));
        }
        found = de.name;
        return make_ready_future();
    });
    co_await h.done();

    if (found.empty()) {
        throw std::runtime_error(fmt::format("failed to load system.tablets from {}: couldn't find table directory", scylla_data_path));
    }
    co_return system_tables_path / found.c_str();
}

mutation_opt read_mutation(sharded<sstable_manager_service>& sst_man,
                           std::filesystem::path table_path,
                           reader_permit permit,
                           std::string_view ks_name,
                           utils::UUID table_id) {
    sharded<sstables::sstable_directory> sst_dirs;
    sst_dirs.start(
        sharded_parameter([&sst_man] { return std::ref(sst_man.local().sst_man); }),
        sharded_parameter([] { return db::system_keyspace::tablets(); }),
        sharded_parameter([] { return db::system_keyspace::tablets()->get_sharder(); }),
        sharded_parameter([] { return make_lw_shared<const data_dictionary::storage_options>(); }),
        table_path.native(),
        sstables::sstable_state::normal,
        sharded_parameter([] { return default_io_error_handler_gen(); })).get();
    auto stop_sst_dirs = deferred_stop(sst_dirs);

    using open_infos_t = std::vector<sstables::foreign_sstable_open_info>;
    auto sstable_open_infos = sst_dirs.map_reduce0(
            [] (sstables::sstable_directory& sst_dir) -> future<std::vector<sstables::foreign_sstable_open_info>> {
               co_await sst_dir.process_sstable_dir(sstables::sstable_directory::process_flags{ .sort_sstables_according_to_owner = false });
               const auto& unsorted_ssts = sst_dir.get_unsorted_sstables();
               open_infos_t open_infos;
               open_infos.reserve(unsorted_ssts.size());
               for (auto& sst : unsorted_ssts) {
                   open_infos.push_back(co_await sst->get_open_info());
               }
               co_return open_infos;
            },
            open_infos_t{},
            [] (open_infos_t lhs, std::vector<sstables::foreign_sstable_open_info> rhs) {
                std::move(lhs.begin(), lhs.end(), std::back_inserter(rhs));
                return lhs;
            }).get();
    if (sstable_open_infos.empty()) {
        logger.warn("no sstables found in {}", table_path);
        return {};
    }

    std::vector<sstables::shared_sstable> sstables;
    sstables.reserve(sstable_open_infos.size());
    for (auto& open_info : sstable_open_infos) {
        auto sst = sst_dirs.local().load_foreign_sstable(open_info).get();
        sstables.push_back(std::move(sst));
    }

    auto schema = db::system_keyspace::tablets();
    auto pk = partition_key::from_deeply_exploded(*schema, {data_value(table_id)});
    auto dk = dht::decorate_key(*schema, pk);
    auto pr = dht::partition_range::make_singular(dk);
    auto ps = partition_slice_builder(*schema)
            .build();

    logger.trace("read_mutations: preparing to read {} sstables for table={}.{}, pr={} ({})\n{}",
            sstable_open_infos.size(),
            schema->ks_name(),
            schema->cf_name(),
            pr,
            table_id,
            sstables);

    std::vector<mutation_reader> readers;
    readers.reserve(sstables.size());
    for (const auto& sst : sstables) {
        readers.emplace_back(sst->make_reader(schema, permit, pr, ps));
    }
    auto reader = make_combined_reader(schema, permit, std::move(readers));
    auto close_reader = deferred_close(reader);

    auto mut_opt = read_mutation_from_mutation_reader(reader).get();

    if (mut_opt) {
        logger.debug("read_mutations: mutation for table={}.{}, pr={} ({})\n{}",
                     schema->ks_name(),
                     schema->cf_name(),
                     pr,
                     table_id,
                     *mut_opt);
    } else {
        logger.debug("read_mutations: empty mutation for table={}.{}, pr={} ({})",
                     schema->ks_name(),
                     schema->cf_name(),
                     pr,
                     table_id);
    }
    return mut_opt;
}

future<utils::UUID> get_table_id(std::filesystem::path scylla_data_path,
                                 std::string_view keyspace_name,
                                 std::string_view table_name) {
    auto path = co_await get_table_directory(scylla_data_path,
                                             keyspace_name,
                                             table_name);
    // the part after "-" is the string representation of the table_id
    //   "$scylla_data_path/system/tablets-fd4f7a4696bd3e7391bf99eb77e82a5c"
    auto fn = path.filename().native();
    auto dash_pos = fn.find_last_of('-');
    assert(dash_pos != fn.npos);
    if (dash_pos == fn.size()) {
        throw std::runtime_error(fmt::format("failed parse system.tablets path {}: bad path", path));
    }
    co_return utils::UUID{fn.substr(dash_pos + 1).c_str()};
}

locator::tablet_replica_set
replica_set_from_row(const query::result_set_row& row, std::string_view name) {
    std::vector<data_value> column = row.get_nonnull<const list_type_impl::native_type&>(sstring{name});
    locator::tablet_replica_set replica_set;
    for (auto& v : column) {
        std::vector<data_value> replica = value_cast<tuple_type_impl::native_type>(v);
        auto host = value_cast<utils::UUID>(replica[0]);
        auto shard = value_cast<int>(replica[1]);
        replica_set.emplace_back(locator::host_id{host}, shard);
    }
    return replica_set;
}

tools::tablets_t do_load_system_tablets(const db::config& dbcfg,
                                        std::filesystem::path scylla_data_path,
                                        std::string_view keyspace_name,
                                        std::string_view table_name) {
    reader_concurrency_semaphore rcs_sem(reader_concurrency_semaphore::no_limits{},
                                         "load_system_tablets",
                                         reader_concurrency_semaphore::register_metrics::no);
    auto stop_semaphore = deferred_stop(rcs_sem);

    sharded<sstable_manager_service> sst_man;
    sst_man.start(std::ref(dbcfg)).get();
    auto stop_sst_man_service = deferred_stop(sst_man);

    auto schema = db::system_keyspace::tablets();
    auto tablets_table_directory = get_table_directory(scylla_data_path,
                                                       db::system_keyspace::NAME,
                                                       schema->cf_name()).get();
    auto table_id = get_table_id(tablets_table_directory, keyspace_name, table_name).get();
    auto mut = read_mutation(
        sst_man, tablets_table_directory,
        rcs_sem.make_tracking_only_permit(schema, "tablets", db::no_timeout, {}),
        db::system_keyspace::NAME,
        table_id);
    if (!mut || mut->partition().row_count() == 0) {
        throw std::runtime_error(fmt::format("failed to find tablets for {}.{}", keyspace_name, table_name));
    }

    auto ks = make_lw_shared<data_dictionary::keyspace_metadata>(keyspace_name,
                                                                 "org.apache.cassandra.locator.LocalStrategy",
                                                                 std::map<sstring, sstring>{},
                                                                 std::nullopt, false);
    db::cql_type_parser::raw_builder ut_builder(*ks);

    tools::tablets_t tablets;
    query::result_set result_set{*mut};
    for (auto& row : result_set.rows()) {
        auto last_token = row.get_nonnull<int64_t>("last_token");
        auto replica_set = replica_set_from_row(row, "replicas");
        tablets.emplace(last_token, std::move(replica_set));
    }
    return tablets;
}

} // anonymous namespace

namespace tools {

future<tablets_t> load_system_tablets(const db::config &dbcfg,
                                      std::filesystem::path scylla_data_path,
                                      std::string_view keyspace_name,
                                      std::string_view table_name) {
    return async([=, &dbcfg] {
        return do_load_system_tablets(dbcfg, scylla_data_path, keyspace_name, table_name);
    });
}

} // namespace tools
