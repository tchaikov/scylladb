/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <seastar/core/coroutine.hh>

#include "consumer.hh"
#include "replica/database.hh"
#include "mutation/mutation_source_metadata.hh"
#include "db/view/view_update_generator.hh"
#include "db/view/view_update_checks.hh"
#include "sstables/sstables.hh"
#include "sstables/sstables_manager.hh"

namespace streaming {

std::function<future<> (flat_mutation_reader_v2)> make_streaming_consumer(sstring origin,
        sharded<replica::database>& db,
        sharded<db::system_distributed_keyspace>& sys_dist_ks,
        sharded<db::view::view_update_generator>& vug,
        uint64_t estimated_partitions,
        stream_reason reason,
        sstables::offstrategy offstrategy) {
    return [&db, &sys_dist_ks, &vug, estimated_partitions, reason, offstrategy, origin = std::move(origin)] (flat_mutation_reader_v2 reader) -> future<> {
        std::exception_ptr ex;
        try {
            auto cf = db.local().find_column_family(reader.schema()).shared_from_this();
            auto use_view_update_path = co_await db::view::check_needs_view_update_path(sys_dist_ks.local(), db.local().get_token_metadata(), *cf, reason);
            //FIXME: for better estimations this should be transmitted from remote
            auto metadata = mutation_source_metadata{};
            auto& cs = cf->get_compaction_strategy();
            const auto adjusted_estimated_partitions = cs.adjust_partition_estimate(metadata, estimated_partitions);
            reader_consumer_v2 consumer =
                    [cf = std::move(cf), adjusted_estimated_partitions, use_view_update_path, &vug, origin = std::move(origin), offstrategy] (flat_mutation_reader_v2 reader) {
                sstables::shared_sstable sst;
                try {
                    sst = use_view_update_path ? cf->make_streaming_staging_sstable() : cf->make_streaming_sstable_for_write();
                } catch (...) {
                    return current_exception_as_future().finally([reader = std::move(reader)] () mutable {
                        return reader.close();
                    });
                }
                schema_ptr s = reader.schema();

                auto cfg = cf->get_sstables_manager().configure_writer(origin);
                cfg.erm = cf->get_effective_replication_map();
                return sst->write_components(std::move(reader), adjusted_estimated_partitions, s,
                                             cfg, encoding_stats{}).then([sst] {
                    return sst->open_data();
                }).then([cf, sst, offstrategy, origin] {
                    if (offstrategy && sstables::repair_origin == origin) {
                        sstables::sstlog.debug("Enabled automatic off-strategy trigger for table {}.{}",
                                cf->schema()->ks_name(), cf->schema()->cf_name());
                        cf->enable_off_strategy_trigger();
                    }
                    return cf->add_sstable_and_update_cache(sst, offstrategy);
                }).then([cf, s, sst, use_view_update_path, &vug]() mutable -> future<> {
                    if (!use_view_update_path) {
                        return make_ready_future<>();
                    }
                    return vug.local().register_staging_sstable(sst, std::move(cf));
                });
            };
            // postpone data segregation to off-strategy compaction if enabled
            if (!offstrategy) {
                consumer = cs.make_consumer(metadata, std::move(consumer));
            }
            co_return co_await consumer(std::move(reader));
        } catch (...) {
            ex = std::current_exception();
        }
        if (ex) {
            co_await reader.close();
            std::rethrow_exception(std::move(ex));
        }
    };
}

}
