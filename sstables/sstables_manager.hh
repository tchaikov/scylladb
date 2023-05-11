/*
 * Copyright (C) 2019-present ScyllaDB
 *
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>

#include "utils/disk-error-handler.hh"
#include "gc_clock.hh"
#include "sstables/sstables.hh"
#include "sstables/shareable_components.hh"
#include "sstables/shared_sstable.hh"
#include "sstables/version.hh"
#include "sstables/component_type.hh"
#include "db/cache_tracker.hh"
#include "locator/host_id.hh"
#include "reader_concurrency_semaphore.hh"
#include "utils/s3/creds.hh"
#include <boost/intrusive/list.hpp>

namespace db {

class system_keyspace;
class large_data_handler;
class config;

}   // namespace db

namespace gms { class feature_service; }

namespace sstables {

class directory_semaphore;
using schema_ptr = lw_shared_ptr<const schema>;
using shareable_components_ptr = lw_shared_ptr<shareable_components>;
using object_storage_config = std::unordered_map<sstring, s3::endpoint_config_ptr>;

static constexpr size_t default_sstable_buffer_size = 128 * 1024;

class sstables_manager {
    using list_type = boost::intrusive::list<sstable,
            boost::intrusive::member_hook<sstable, sstable::manager_link_type, &sstable::_manager_link>,
            boost::intrusive::constant_time_size<false>>;
private:
    db::large_data_handler& _large_data_handler;
    const db::config& _db_config;
    gms::feature_service& _features;
    // _sstables_format is the format used for writing new sstables.
    // Here we set its default value, but if we discover that all the nodes
    // in the cluster support a newer format, _sstables_format will be set to
    // that format. read_sstables_format() also overwrites _sstables_format
    // if an sstable format was chosen earlier (and this choice was persisted
    // in the system table).
    sstable_version_types _format = sstable_version_types::mc;

    // _active and _undergoing_close are used in scylla-gdb.py to fetch all sstables
    // on current shard using "scylla sstables" command. If those fields are renamed,
    // update scylla-gdb.py as well.
    list_type _active;
    list_type _undergoing_close;

    bool _closing = false;
    promise<> _done;
    cache_tracker& _cache_tracker;

    reader_concurrency_semaphore _sstable_metadata_concurrency_sem;
    directory_semaphore& _dir_semaphore;
    seastar::shared_ptr<db::system_keyspace> _sys_ks;
    object_storage_config _object_storage_config;

public:
    explicit sstables_manager(db::large_data_handler& large_data_handler, const db::config& dbcfg, gms::feature_service& feat, cache_tracker&, size_t available_memory, directory_semaphore& dir_sem, object_storage_config oscfg = {});
    virtual ~sstables_manager();

    // Constructs a shared sstable
    shared_sstable make_sstable(schema_ptr schema,
            const data_dictionary::storage_options& storage, // FIXME -- move dir on options
            sstring dir,
            generation_type generation,
            sstable_version_types v = get_highest_sstable_version(),
            sstable_format_types f = sstable_format_types::big,
            gc_clock::time_point now = gc_clock::now(),
            io_error_handler_gen error_handler_gen = default_io_error_handler_gen(),
            size_t buffer_size = default_sstable_buffer_size);

    s3::endpoint_config_ptr get_endpoint_config(sstring endpoint) const {
        return _object_storage_config.at(endpoint);
    }

    void update_object_storage_config(object_storage_config cfg);

    virtual sstable_writer_config configure_writer(sstring origin) const;
    const gms::feature_service& feature_service() const { return _features; }
    const db::config& config() const { return _db_config; }
    cache_tracker& get_cache_tracker() { return _cache_tracker; }

    void set_format(sstable_version_types format) noexcept { _format = format; }
    sstables::sstable::version_types get_highest_supported_format() const noexcept { return _format; }

    const locator::host_id& get_local_host_id() const;

    reader_concurrency_semaphore& sstable_metadata_concurrency_sem() noexcept { return _sstable_metadata_concurrency_sem; }

    // Wait until all sstables managed by this sstables_manager instance
    // (previously created by make_sstable()) have been disposed of:
    //   - if they were marked for deletion, the files are deleted
    //   - in any case, the open file handles are closed
    //   - all memory resources are freed
    //
    // Note that close() will not complete until all references to all
    // sstables have been destroyed.
    future<> close();
    directory_semaphore& dir_semaphore() noexcept { return _dir_semaphore; }

    void plug_system_keyspace(db::system_keyspace& sys_ks) noexcept;
    void unplug_system_keyspace() noexcept;

    // Only for sstable::storage usage
    db::system_keyspace& system_keyspace() const noexcept {
        assert(_sys_ks && "System keyspace is not plugged");
        return *_sys_ks;
    }

private:
    void add(sstable* sst);
    // Transition the sstable to the "inactive" state. It has no
    // visible references at this point, and only waits for its
    // files to be deleted (if necessary) and closed.
    void deactivate(sstable* sst);
    void remove(sstable* sst);
    void maybe_done();

    static constexpr size_t max_count_sstable_metadata_concurrent_reads{10};
    // Allow at most 10% of memory to be filled with such reads.
    size_t max_memory_sstable_metadata_concurrent_reads(size_t available_memory) { return available_memory * 0.1; }
private:
    db::large_data_handler& get_large_data_handler() const {
        return _large_data_handler;
    }
    friend class sstable;
};

}   // namespace sstables
