/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <type_traits>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/util/file.hh>
#include <boost/range/adaptor/map.hpp>
#include <boost/algorithm/string.hpp>
#include "sstables/sstable_directory.hh"
#include "sstables/sstables.hh"
#include "sstables/sstables_manager.hh"
#include "compaction/compaction_manager.hh"
#include "log.hh"
#include "sstable_directory.hh"
#include "utils/lister.hh"
#include "replica/database.hh"

static logging::logger dirlog("sstable_directory");

namespace sstables {

bool manifest_json_filter(const fs::path&, const directory_entry& entry) {
    // Filter out directories. If type of the entry is unknown - check its name.
    if (entry.type.value_or(directory_entry_type::regular) != directory_entry_type::directory && (entry.name == "manifest.json" || entry.name == "schema.cql")) {
        return false;
    }

    return true;
}

sstable_directory::components_lister::components_lister(std::filesystem::path dir)
        : _lister(dir, lister::dir_entry_types::of<directory_entry_type::regular>(), &manifest_json_filter)
        , _state(std::make_unique<scan_state>())
{
}

future<sstring> sstable_directory::components_lister::get() {
    auto de = co_await _lister.get();
    co_return sstring(de ? de->name : "");
}

future<> sstable_directory::components_lister::close() {
    return _lister.close();
}

sstable_directory::sstable_directory(sstables_manager& manager,
        schema_ptr schema,
        fs::path sstable_dir,
        ::io_priority_class io_prio,
        io_error_handler_gen error_handler_gen)
    : _manager(manager)
    , _schema(std::move(schema))
    , _sstable_dir(std::move(sstable_dir))
    , _io_priority(std::move(io_prio))
    , _error_handler_gen(error_handler_gen)
    , _lister(_manager.get_components_lister(_sstable_dir))
    , _unshared_remote_sstables(smp::count)
{}

void sstable_directory::components_lister::handle(sstables::entry_descriptor desc, fs::path filename) {
    if ((generation_value(desc.generation) % smp::count) != this_shard_id()) {
        return;
    }

    dirlog.trace("for SSTable directory, scanning {}", filename);
    _state->generations_found.emplace(desc.generation, filename);

    switch (desc.component) {
    case component_type::TemporaryStatistics:
        // We generate TemporaryStatistics when we rewrite the Statistics file,
        // for instance on mutate_level. We should delete it - so we mark it for deletion
        // here, but just the component. The old statistics file should still be there
        // and we'll go with it.
        _state->files_for_removal.insert(filename.native());
        break;
    case component_type::TOC:
        _state->descriptors.emplace(desc.generation, std::move(desc));
        break;
    case component_type::TemporaryTOC:
        _state->temp_toc_found.push_back(std::move(desc));
        break;
    default:
        // Do nothing, and will validate when trying to load the file.
        break;
    }
}

void sstable_directory::validate(sstables::shared_sstable sst, process_flags flags) const {
    schema_ptr s = sst->get_schema();
    if (s->is_counter() && !sst->has_scylla_component()) {
        sstring error = "Direct loading non-Scylla SSTables containing counters is not supported.";
        if (flags.enable_dangerous_direct_import_of_cassandra_counters) {
            dirlog.info("{} But trying to continue on user's request.", error);
        } else {
            dirlog.error("{} Use sstableloader instead.", error);
            throw std::runtime_error(fmt::format("{} Use sstableloader instead.", error));
        }
    }
    if (s->is_view() && !flags.allow_loading_materialized_view) {
        throw std::runtime_error("Loading Materialized View SSTables is not supported. Re-create the view instead.");
    }
    if (!sst->is_uploaded()) {
        sst->validate_originating_host_id();
    }
}

future<sstables::shared_sstable> sstable_directory::load_sstable(sstables::entry_descriptor desc) const {
    auto sst = _manager.make_sstable(_schema, _sstable_dir.native(), desc.generation, desc.version, desc.format, gc_clock::now(), _error_handler_gen);
    co_await sst->load(_io_priority);
    co_return sst;
}

future<sstables::shared_sstable> sstable_directory::load_sstable(sstables::entry_descriptor desc, process_flags flags) const {
    auto sst = co_await load_sstable(std::move(desc));
    validate(sst, flags);
    if (flags.need_mutate_level) {
        dirlog.trace("Mutating {} to level 0\n", sst->get_filename());
        co_await sst->mutate_sstable_level(0);
    }
    co_return sst;
}

future<>
sstable_directory::process_descriptor(sstables::entry_descriptor desc, process_flags flags) {
    if (desc.version > _max_version_seen) {
        _max_version_seen = desc.version;
    }

    if (flags.sort_sstables_according_to_owner) {
        co_await sort_sstable(std::move(desc), flags);
    } else {
        dirlog.debug("Added {} to unsorted sstables list", sstable_filename(desc));
        _unsorted_sstables.push_back(co_await load_sstable(std::move(desc), flags));
    }
}

future<>
sstable_directory::sort_sstable(sstables::entry_descriptor desc, process_flags flags) {
    auto sst = co_await load_sstable(desc, flags);
    sstables::foreign_sstable_open_info info = co_await sst->get_open_info();

    auto shards = sst->get_shards_for_this_sstable();
    if (shards.size() == 1) {
        if (shards[0] == this_shard_id()) {
            dirlog.trace("{} identified as a local unshared SSTable", sst->get_filename());
            _unshared_local_sstables.push_back(sst);
        } else {
            dirlog.trace("{} identified as a remote unshared SSTable", sst->get_filename());
            _unshared_remote_sstables[shards[0]].push_back(std::move(info));
        }
    } else {
        dirlog.trace("{} identified as a shared SSTable", sst->get_filename());
        _shared_sstable_info.push_back(std::move(info));
    }
}

sstring sstable_directory::sstable_filename(const sstables::entry_descriptor& desc) const {
    return sstable::filename(_sstable_dir.native(), _schema->ks_name(), _schema->cf_name(), desc.version, desc.generation, desc.format, component_type::Data);
}

generation_type
sstable_directory::highest_generation_seen() const {
    return _max_generation_seen;
}

sstables::sstable_version_types
sstable_directory::highest_version_seen() const {
    return _max_version_seen;
}

future<> sstable_directory::process_sstable_dir(process_flags flags) {
    dirlog.debug("Start processing directory {} for SSTables", _sstable_dir);
    return _lister->process(*this, _sstable_dir, flags);
}

future<> sstable_directory::components_lister::process(sstable_directory& directory, fs::path location, process_flags flags) {
    // It seems wasteful that each shard is repeating this scan, and to some extent it is.
    // However, we still want to open the files and especially call process_dir() in a distributed
    // fashion not to overload any shard. Also in the common case the SSTables will all be
    // unshared and be on the right shard based on their generation number. In light of that there are
    // two advantages of having each shard repeat the directory listing:
    //
    // - The directory listing part already interacts with data_structures inside scan_state. We
    //   would have to either transfer a lot of file information among shards or complicate the code
    //   to make sure they all update their own version of scan_state and then merge it.
    // - If all shards scan in parallel, they can start loading sooner. That is faster than having
    //   a separate step to fetch all files, followed by another step to distribute and process.

    std::exception_ptr ex;
    try {
        while (true) {
            sstring name = co_await get();
            if (name == "") {
                break;
            }
            auto comps = sstables::entry_descriptor::make_descriptor(location.native(), name);
            handle(std::move(comps), location / name);
        }
    } catch (...) {
        ex = std::current_exception();
    }
    co_await close();
    if (ex) {
        dirlog.debug("Could not process sstable directory {}: {}", location, ex);
        // FIXME: waiting for https://github.com/scylladb/seastar/pull/1090
        // co_await coroutine::return_exception(std::move(ex));
        std::rethrow_exception(std::move(ex));
    }

    // Always okay to delete files with a temporary TOC. We want to do it before we process
    // the generations seen: it's okay to reuse those generations since the files will have
    // been deleted anyway.
    for (auto& desc: _state->temp_toc_found) {
        auto range = _state->generations_found.equal_range(desc.generation);
        for (auto it = range.first; it != range.second; ++it) {
            auto& path = it->second;
            dirlog.trace("Scheduling to remove file {}, from an SSTable with a Temporary TOC", path.native());
            _state->files_for_removal.insert(path.native());
        }
        _state->generations_found.erase(range.first, range.second);
        _state->descriptors.erase(desc.generation);
    }

    directory._max_generation_seen =  boost::accumulate(_state->generations_found | boost::adaptors::map_keys, generation_from_value(0), [] (generation_type a, generation_type b) {
        return std::max<generation_type>(a, b);
    });

    dirlog.debug("After {} scanned, seen generation {}. {} descriptors found, {} different files found ",
            location, directory._max_generation_seen, _state->descriptors.size(), _state->generations_found.size());

    // _descriptors is everything with a TOC. So after we remove this, what's left is
    // SSTables for which a TOC was not found.
    co_await directory.parallel_for_each_restricted(_state->descriptors, [this, flags, &directory] (std::pair<const generation_type, sstables::entry_descriptor>& t) {
        auto& desc = std::get<1>(t);
        _state->generations_found.erase(desc.generation);
        // This will try to pre-load this file and throw an exception if it is invalid
        return directory.process_descriptor(std::move(desc), flags);
    });

    // For files missing TOC, it depends on where this is coming from.
    // If scylla was supposed to have generated this SSTable, this is not okay and
    // we refuse to proceed. If this coming from, say, an import, then we just delete,
    // log and proceed.
    for (auto& path : _state->generations_found | boost::adaptors::map_values) {
        if (flags.throw_on_missing_toc) {
            throw sstables::malformed_sstable_exception(format("At directory: {}: no TOC found for SSTable {}!. Refusing to boot", location.native(), path.native()));
        } else {
            dirlog.info("Found incomplete SSTable {} at directory {}. Removing", path.native(), location.native());
            _state->files_for_removal.insert(path.native());
        }
    }
}

future<> sstable_directory::commit_directory_changes() {
    return _lister->commit().finally([x = std::move(_lister)] {});
}

future<> sstable_directory::components_lister::commit() {
    // Remove all files scheduled for removal
    return parallel_for_each(std::exchange(_state->files_for_removal, {}), [] (sstring path) {
        dirlog.info("Removing file {}", path);
        return remove_file(std::move(path));
    });
}

future<>
sstable_directory::move_foreign_sstables(sharded<sstable_directory>& source_directory) {
    return parallel_for_each(boost::irange(0u, smp::count), [this, &source_directory] (unsigned shard_id) mutable {
        auto info_vec = std::exchange(_unshared_remote_sstables[shard_id], {});
        if (info_vec.empty()) {
            return make_ready_future<>();
        }
        // Should be empty, since an SSTable that belongs to this shard is not remote.
        assert(shard_id != this_shard_id());
        dirlog.debug("Moving {} unshared SSTables to shard {} ", info_vec.size(), shard_id);
        return source_directory.invoke_on(shard_id, &sstables::sstable_directory::load_foreign_sstables, std::move(info_vec));
    });
}

future<shared_sstable> sstable_directory::load_foreign_sstable(foreign_sstable_open_info& info) {
    auto sst = _manager.make_sstable(_schema, _sstable_dir.native(), info.generation, info.version, info.format, gc_clock::now(), _error_handler_gen);
    co_await sst->load(std::move(info));
    co_return sst;
}

future<>
sstable_directory::load_foreign_sstables(sstable_info_vector info_vec) {
    return parallel_for_each_restricted(info_vec, [this] (sstables::foreign_sstable_open_info& info) {
        return load_foreign_sstable(info).then([this] (auto sst) {
            _unshared_local_sstables.push_back(sst);
            return make_ready_future<>();
        });
    });
}

future<>
sstable_directory::remove_sstables(std::vector<sstables::shared_sstable> sstlist) {
    dirlog.debug("Removing {} SSTables", sstlist.size());
    return parallel_for_each(std::move(sstlist), [] (const sstables::shared_sstable& sst) {
        dirlog.trace("Removing SSTable {}", sst->get_filename());
        return sst->unlink().then([sst] {});
    });
}

future<>
sstable_directory::collect_output_unshared_sstables(std::vector<sstables::shared_sstable> resharded_sstables, can_be_remote remote_ok) {
    dirlog.debug("Collecting {} output SSTables (remote={})", resharded_sstables.size(), remote_ok);
    return parallel_for_each(std::move(resharded_sstables), [this, remote_ok] (sstables::shared_sstable sst) {
        auto shards = sst->get_shards_for_this_sstable();
        assert(shards.size() == 1);
        auto shard = shards[0];

        if (shard == this_shard_id()) {
            dirlog.trace("Collected output SSTable {} already local", sst->get_filename());
            _unshared_local_sstables.push_back(std::move(sst));
            return make_ready_future<>();
        }

        if (!remote_ok) {
            return make_exception_future<>(std::runtime_error("Unexpected remote sstable"));
        }

        dirlog.trace("Collected output SSTable {} is remote. Storing it", sst->get_filename());
        return sst->get_open_info().then([this, shard, sst] (sstables::foreign_sstable_open_info info) {
            _unshared_remote_sstables[shard].push_back(std::move(info));
            return make_ready_future<>();
        });
    });
}

future<>
sstable_directory::remove_unshared_sstables(std::vector<sstables::shared_sstable> sstlist) {
    // When removing input sstables from reshaping: Those SSTables used to be in the unshared local
    // list. So not only do we have to remove them, we also have to update the list. Because we're
    // dealing with a vector it's easier to just reconstruct the list.
    dirlog.debug("Removing {} unshared SSTables", sstlist.size());
    return do_with(std::move(sstlist), std::unordered_set<sstables::shared_sstable>(),
            [this] (std::vector<sstables::shared_sstable>& sstlist, std::unordered_set<sstables::shared_sstable>& exclude) {

        for (auto& sst : sstlist) {
            exclude.insert(sst);
        }

        auto old = std::exchange(_unshared_local_sstables, {});

        for (auto& sst : old) {
            if (!exclude.contains(sst)) {
                _unshared_local_sstables.push_back(sst);
            }
        }

        // Do this last for exception safety. If there is an exception on unlink we
        // want to at least leave the SSTable unshared list in a sane state.
        return remove_sstables(std::move(sstlist)).then([] {
            dirlog.debug("Finished removing all SSTables");
        });
    });
}


future<>
sstable_directory::do_for_each_sstable(std::function<future<>(sstables::shared_sstable)> func) {
    return parallel_for_each_restricted(_unshared_local_sstables, std::move(func));
}

template <typename Container, typename Func>
requires std::is_invocable_r_v<future<>, Func, typename std::decay_t<Container>::value_type&>
future<>
sstable_directory::parallel_for_each_restricted(Container&& C, Func&& func) {
    return do_with(std::move(C), std::move(func), [this] (Container& c, Func& func) mutable {
      return max_concurrent_for_each(c, _manager.dir_semaphore()._concurrency, [this, &func] (auto& el) mutable {
        return with_semaphore(_manager.dir_semaphore()._sem, 1, [&func,  el = std::move(el)] () mutable {
            return func(el);
        });
      });
    });
}

void
sstable_directory::store_phaser(utils::phased_barrier::operation op) {
    _operation_barrier.emplace(std::move(op));
}

sstable_directory::sstable_info_vector
sstable_directory::retrieve_shared_sstables() {
    return std::exchange(_shared_sstable_info, {});
}

bool sstable_directory::compare_sstable_storage_prefix(const sstring& prefix_a, const sstring& prefix_b) noexcept {
    size_t size_a = prefix_a.size();
    if (prefix_a.back() == '/') {
        size_a--;
    }
    size_t size_b = prefix_b.size();
    if (prefix_b.back() == '/') {
        size_b--;
    }
    return size_a == size_b && sstring::traits_type::compare(prefix_a.begin(), prefix_b.begin(), size_a) == 0;
}

future<> sstable_directory::delete_atomically(std::vector<shared_sstable> ssts) {
    if (ssts.empty()) {
        return make_ready_future<>();
    }
    return seastar::async([ssts = std::move(ssts)] {
        shared_sstable first = nullptr;
        min_max_tracker<generation_type> gen_tracker;

        for (const auto& sst : ssts) {
            gen_tracker.update(sst->generation());

            if (first == nullptr) {
                first = sst;
            } else {
                // All sstables are assumed to be in the same column_family, hence
                // sharing their base directory. Since lexicographical comparison of
                // paths is not the same as their actualy equivalence, this should
                // rather check for fs::equivalent call on _storage.prefix()-s. But
                // since we know that the worst thing filesystem storage driver can
                // do is to prepend/drop the trailing slash, it should be enough to
                // compare prefixes of both ... prefixes
                assert(compare_sstable_storage_prefix(first->_storage.prefix(), sst->_storage.prefix()));
            }
        }

        sstring pending_delete_dir = first->_storage.prefix() + "/" + sstable::pending_delete_dir_basename();
        sstring pending_delete_log = format("{}/sstables-{}-{}.log", pending_delete_dir, gen_tracker.min(), gen_tracker.max());
        sstring tmp_pending_delete_log = pending_delete_log + ".tmp";
        sstlog.trace("Writing {}", tmp_pending_delete_log);
        try {
            touch_directory(pending_delete_dir).get();
            auto oflags = open_flags::wo | open_flags::create | open_flags::exclusive;
            // Create temporary pending_delete log file.
            auto f = open_file_dma(tmp_pending_delete_log, oflags).get0();
            // Write all toc names into the log file.
            auto out = make_file_output_stream(std::move(f), 4096).get0();
            auto close_out = deferred_close(out);

            for (const auto& sst : ssts) {
                auto toc = sst->component_basename(component_type::TOC);
                out.write(toc).get();
                out.write("\n").get();
            }

            out.flush().get();
            close_out.close_now();

            auto dir_f = open_directory(pending_delete_dir).get0();
            // Once flushed and closed, the temporary log file can be renamed.
            rename_file(tmp_pending_delete_log, pending_delete_log).get();

            // Guarantee that the changes above reached the disk.
            dir_f.flush().get();
            dir_f.close().get();
            sstlog.debug("{} written successfully.", pending_delete_log);
        } catch (...) {
            sstlog.warn("Error while writing {}: {}. Ignoring.", pending_delete_log, std::current_exception());
        }

        parallel_for_each(ssts, [] (shared_sstable sst) {
            return sst->unlink();
        }).get();

        // Once all sstables are deleted, the log file can be removed.
        // Note: the log file will be removed also if unlink failed to remove
        // any sstable and ignored the error.
        try {
            remove_file(pending_delete_log).get();
            sstlog.debug("{} removed.", pending_delete_log);
        } catch (...) {
            sstlog.warn("Error removing {}: {}. Ignoring.", pending_delete_log, std::current_exception());
        }
    });
}

// FIXME: Go through maybe_delete_large_partitions_entry on recovery
// since this is an indication we crashed in the middle of delete_atomically
future<> sstable_directory::replay_pending_delete_log(fs::path pending_delete_log) {
    sstlog.debug("Reading pending_deletes log file {}", pending_delete_log);
    fs::path pending_delete_dir = pending_delete_log.parent_path();
    assert(sstable::is_pending_delete_dir(pending_delete_dir));
    try {
        sstring sstdir = pending_delete_dir.parent_path().native();
        auto text = co_await seastar::util::read_entire_file_contiguous(pending_delete_log);

        sstring all(text.begin(), text.end());
        std::vector<sstring> basenames;
        boost::split(basenames, all, boost::is_any_of("\n"), boost::token_compress_on);
        auto tocs = boost::copy_range<std::vector<sstring>>(basenames | boost::adaptors::filtered([] (auto&& basename) { return !basename.empty(); }));
        co_await parallel_for_each(tocs, [&sstdir] (const sstring& name) {
            return remove_by_toc_name(sstdir + "/" + name);
        });
        sstlog.debug("Replayed {}, removing", pending_delete_log);
        co_await remove_file(pending_delete_log.native());
    } catch (...) {
        sstlog.warn("Error replaying {}: {}. Ignoring.", pending_delete_log, std::current_exception());
    }
}

}
