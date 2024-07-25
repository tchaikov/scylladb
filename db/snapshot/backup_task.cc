/*
 * Copyright (C) 2024-present ScyllaDB
 *
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utils/lister.hh"
#include "utils/s3/client.hh"
#include "replica/database.hh"
#include "db/config.hh"
#include "db/snapshot-ctl.hh"
#include "db/snapshot/backup_task.hh"
#include "schema/schema_fwd.hh"
#include "sstables/sstables.hh"

extern logging::logger snap_log;

namespace db::snapshot {

backup_task_impl::backup_task_impl(tasks::task_manager::module_ptr module, snapshot_ctl& ctl, shared_ptr<s3::client> client, sstring bucket, sstring ks, sstring snapshot_name) noexcept
    : tasks::task_manager::task::impl(module, tasks::task_id::create_random_id(), 0, "node", ks, "", "", tasks::task_id::create_null_id())
    , _snap_ctl(ctl)
    , _client(std::move(client))
    , _bucket(std::move(bucket))
    , _ks(std::move(ks))
    , _snapshot_name(std::move(snapshot_name))
{}

std::string backup_task_impl::type() const {
    return "backup";
}

tasks::is_internal backup_task_impl::is_internal() const noexcept {
    return tasks::is_internal::no;
}

static std::pair<sstring, table_id> extract_cf_name_and_uuid(const sstring& directory_name) {
    // cf directory is of the form: 'cf_name-uuid'
    // uuid is assumed to be exactly 32 hex characters wide.
    constexpr size_t uuid_size = 32;
    ssize_t pos = directory_name.size() - uuid_size - 1;
    if (pos <= 0 || directory_name[pos] != '-') {
        on_internal_error(snap_log, format("table directory entry name '{}' is invalid: no '-' separator found at pos {}", directory_name, pos));
    }
    return std::make_pair(directory_name.substr(0, pos), table_id(utils::UUID(directory_name.substr(pos + 1))));
}

void backup_task_impl::run_in_thread(sstring data_dir) {
    std::exception_ptr ex;
    //
    // The keyspace data directories and their snapshots are arranged as follows:
    //
    //  <data dir>
    //  |- <keyspace name1>
    //  |  |- <column family name1>
    //  |     |- snapshots
    //  |        |- <snapshot name1>
    //  |          |- <snapshot file1>
    //  |          |- <snapshot file2>
    //  |          |- ...
    //  |        |- <snapshot name2>
    //  |        |- ...
    //  |  |- <column family name2>
    //  |  |- ...
    //  |- <keyspace name2>
    //  |- ...
    //
    auto ks_dir = fs::path(data_dir) / _ks;
    auto ks_dir_lister = directory_lister(ks_dir, lister::dir_entry_types::of<directory_entry_type::directory>());
    auto close_ks_dir_lister = deferred_close(ks_dir_lister);

    snap_log.trace("Scan keyspace dir {}", ks_dir.native());
    while (auto table_ent = ks_dir_lister.get().get()) {
        auto snapshot_dir = ks_dir/ table_ent->name / sstables::snapshots_dir / _snapshot_name;
        auto has_snapshot = file_exists(snapshot_dir.native()).get();

        snap_log.trace("Check table snapshot dir {} (exists={})", snapshot_dir.native(), has_snapshot);
        if (!has_snapshot) {
            continue;
        }

        auto cf_name_and_uuid = extract_cf_name_and_uuid(table_ent->name);
        auto snapshot_dir_lister = directory_lister(snapshot_dir, lister::dir_entry_types::of<directory_entry_type::regular>());
        auto close_snapshot_dir_lister = deferred_close(snapshot_dir_lister);

        while (auto component_ent = snapshot_dir_lister.get().get()) {
            auto gh = _uploads.hold();
            auto component_name = snapshot_dir / component_ent->name;
            auto destination = fmt::format("/{}/{}/{}/{}", _bucket, cf_name_and_uuid.first, _snapshot_name, component_ent->name);
            snap_log.trace("Upload {} to {}", component_name.native(), destination);
            std::ignore = _client->upload_file(component_name, destination).handle_exception([this, comp = component_name] (auto ex) {
                snap_log.error("Error uploading {}: {}", comp.native(), ex);
                _ex = std::move(ex);
            }).finally([gh = std::move(gh)] {});
            thread::maybe_yield();
            if (_ex) {
                break;
            }
        }
    }
}

future<> backup_task_impl::run() {
    co_await _snap_ctl.run_snapshot_list_operation(coroutine::lambda([this] -> future<> {
        std::vector<sstring> data_dirs = _snap_ctl._db.local().get_config().data_file_directories();
        co_await coroutine::parallel_for_each(data_dirs, [this] (auto& d) {
            return async([this, d] {
                try {
                    run_in_thread(d);
                } catch (...) {
                    _ex = std::current_exception();
                    snap_log.warn("Error walking snapshots in {}: {}", d, _ex);
                }
            });
        });
        snap_log.trace("Waiting for uploads to finish");
        co_await _uploads.close();
        if (_ex) {
            snap_log.error("Backup finished with error");
            std::rethrow_exception(_ex);
        }
    }));
    snap_log.info("Finished backup");
}

} // db::snapshot namespace
