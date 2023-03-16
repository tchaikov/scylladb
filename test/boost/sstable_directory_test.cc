/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */


#include "test/lib/scylla_test_case.hh"
#include <seastar/core/sstring.hh>
#include "sstables/shared_sstable.hh"
#include "sstables/sstable_directory.hh"
#include "replica/distributed_loader.hh"
#include "test/lib/sstable_utils.hh"
#include "test/lib/cql_test_env.hh"
#include "test/lib/tmpdir.hh"
#include "test/lib/key_utils.hh"
#include "db/config.hh"
#include "utils/lister.hh"

#include "fmt/format.h"

class distributed_loader_for_tests {
public:
    static future<> process_sstable_dir(sharded<sstables::sstable_directory>& dir, sstable_directory::process_flags flags) {
        return replica::distributed_loader::process_sstable_dir(dir, flags);
    }
    static future<> lock_table(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring cf_name) {
        return replica::distributed_loader::lock_table(dir, db, std::move(ks_name), std::move(cf_name));
    }
    static future<> reshard(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator) {
        return replica::distributed_loader::reshard(dir, db, std::move(ks_name), std::move(table_name), std::move(creator), default_priority_class());
    }
};

schema_ptr test_table_schema() {
    static thread_local auto s = [] {
        schema_builder builder(make_shared_schema(
                generate_legacy_id("ks", "cf"), "ks", "cf",
        // partition key
        {{"p", bytes_type}},
        // clustering key
        {},
        // regular columns
        {{"c", int32_type}},
        // static columns
        {},
        // regular column name type
        bytes_type,
        // comment
        ""
       ));
       return builder.build(schema_builder::compact_storage::no);
    }();
    return s;
}

using namespace sstables;

sstables::shared_sstable
make_sstable_for_this_shard(std::function<sstables::shared_sstable()> sst_factory) {
    auto s = test_table_schema();
    auto key = tests::generate_partition_key(s);
    mutation m(s, key);
    m.set_clustered_cell(clustering_key::make_empty(), bytes("c"), data_value(int32_t(0)), api::timestamp_type(0));
    return make_sstable_containing(sst_factory, {m});
}

/// Create a shared SSTable belonging to all shards for the following schema: "create table cf (p text PRIMARY KEY, c int)"
///
/// Arguments passed to the function are passed to table::make_sstable
template <typename... Args>
sstables::shared_sstable
make_sstable_for_all_shards(replica::database& db, replica::table& table, fs::path sstdir, int64_t generation) {
    // Unlike the previous helper, we'll assume we're in a thread here. It's less flexible
    // but the users are usually in a thread, and rewrite_toc_without_scylla_component requires
    // a thread. We could fix that, but deferring that for now.
    auto s = table.schema();
    auto mt = make_lw_shared<replica::memtable>(s);
    for (shard_id shard = 0; shard < smp::count; ++shard) {
        auto key = tests::generate_partition_key(s, shard);
        mutation m(s, key);
        m.set_clustered_cell(clustering_key::make_empty(), bytes("c"), data_value(int32_t(0)), api::timestamp_type(0));
        mt->apply(std::move(m));
    }
    auto sst = table.get_sstables_manager().make_sstable(s, sstdir.native(), generation_from_value(generation++),
            sstables::get_highest_sstable_version(), sstables::sstable::format_types::big);
    write_memtable_to_sstable(*mt, sst, table.get_sstables_manager().configure_writer("test")).get();
    mt->clear_gently().get();
    // We can't write an SSTable with bad sharding, so pretend
    // it came from Cassandra
    sstables::test(sst).remove_component(sstables::component_type::Scylla).get();
    sstables::test(sst).rewrite_toc_without_scylla_component();
    return sst;
}

sstables::shared_sstable new_sstable(sstables::test_env& env, fs::path dir, int64_t gen) {
    return env.manager().make_sstable(test_table_schema(), dir.native(), generation_from_value(gen),
                sstables::sstable_version_types::mc, sstables::sstable_format_types::big,
                gc_clock::now(), default_io_error_handler_gen(), default_sstable_buffer_size);
}

// there is code for this in distributed_loader.cc but this is so simple it is not worth polluting
// the public namespace for it. Repeat it here.
inline future<int64_t>
highest_generation_seen(sharded<sstables::sstable_directory>& dir) {
    return dir.map_reduce0(std::mem_fn(&sstable_directory::highest_generation_seen), generation_from_value(0), [] (generation_type a, generation_type b) {
        return std::max<generation_type>(a, b);
    }).then([] (generation_type gen) {
        return int64_t(gen);
    });
}

class wrapped_test_env {
    std::function<sstables::sstables_manager* ()> _get_mgr;
public:
    wrapped_test_env(sstables::test_env& env) : _get_mgr([m = &env.manager()] { return m; }) {}
    // This variant this transportable across shards
    wrapped_test_env(sharded<sstables::test_env>& env) : _get_mgr([s = &env] { return &s->local().manager(); }) {}
    // This variant this transportable across shards
    wrapped_test_env(cql_test_env& env) : _get_mgr([&env] { return &env.db().local().get_user_sstables_manager(); }) {}
    sstables_manager& get_manager() { return *_get_mgr(); }
};

// Called from a seastar thread
static void with_sstable_directory(
    std::filesystem::path path,
    wrapped_test_env env_wrap,
    noncopyable_function<void (sharded<sstable_directory>&)> func) {

    sharded<sstables::directory_semaphore> sstdir_sem;
    sstdir_sem.start(1).get();
    auto stop_sstdir_sem = defer([&sstdir_sem] {
        sstdir_sem.stop().get();
    });

    sharded<sstable_directory> sstdir;
    auto stop_sstdir = defer([&sstdir] {
        // The func is allowed to stop sstdir, and some tests actually do it
        if (sstdir.local_is_initialized()) {
            sstdir.stop().get();
        }
    });

    sstdir.start(seastar::sharded_parameter([&env_wrap] { return std::ref(env_wrap.get_manager()); }),
            seastar::sharded_parameter([] { return test_table_schema(); }),
            std::move(path), default_priority_class(),
            default_io_error_handler_gen()).get();

    func(sstdir);
}

SEASTAR_TEST_CASE(sstable_directory_test_table_simple_empty_directory_scan) {
    return sstables::test_env::do_with_async([] (test_env& env) {
        auto& dir = env.tempdir();

        // Write a manifest file to make sure it's ignored
        auto manifest = dir.path() / "manifest.json";
        auto f = open_file_dma(manifest.native(), open_flags::wo | open_flags::create | open_flags::truncate).get0();
        f.close().get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            distributed_loader_for_tests::process_sstable_dir(sstdir, {}).get();
            int64_t max_generation_seen = highest_generation_seen(sstdir).get0();
            // No generation found on empty directory.
            BOOST_REQUIRE_EQUAL(max_generation_seen, 0);
        });
    });
}

// Test unrecoverable SSTable: missing a file that is expected in the TOC.
SEASTAR_TEST_CASE(sstable_directory_test_table_scan_incomplete_sstables) {
    return sstables::test_env::do_with_async([] (test_env& env) {
        auto& dir = env.tempdir();
        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir.path(), 1));

        // Now there is one sstable to the upload directory, but it is incomplete and one component is missing.
        // We should fail validation and leave the directory untouched
        remove_file(test::filename(*sst, sstables::component_type::Statistics).native()).get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            auto expect_malformed_sstable = distributed_loader_for_tests::process_sstable_dir(sstdir, {});
            BOOST_REQUIRE_THROW(expect_malformed_sstable.get(), sstables::malformed_sstable_exception);
        });
    });
}

// Test scanning a directory with unrecognized file
// reproducing https://github.com/scylladb/scylla/issues/10697
SEASTAR_TEST_CASE(sstable_directory_test_table_scan_invalid_file) {
    return sstables::test_env::do_with_async([] (test_env& env) {
        auto& dir = env.tempdir();
        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir.path(), 1));

        // Add a bogus file in the sstables directory
        auto name = dir.path() / "bogus";
        auto f = open_file_dma(name.native(), open_flags::rw | open_flags::create | open_flags::truncate).get0();
        f.close().get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
                auto expect_malformed_sstable = distributed_loader_for_tests::process_sstable_dir(sstdir, {});
                BOOST_REQUIRE_THROW(expect_malformed_sstable.get(), sstables::malformed_sstable_exception);
        });
    });
}

// Test always-benign incomplete SSTable: temporaryTOC found
SEASTAR_TEST_CASE(sstable_directory_test_table_temporary_toc) {
    return sstables::test_env::do_with_async([] (test_env& env) {
        auto& dir = env.tempdir();
        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir.path(), 1));
        rename_file(test::filename(*sst, sstables::component_type::TOC).native(), test::filename(*sst, sstables::component_type::TemporaryTOC).native()).get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            auto expect_ok = distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true });
            BOOST_REQUIRE_NO_THROW(expect_ok.get());
        });
    });
}

// Test always-benign incomplete SSTable: with extraneous temporaryTOC found
SEASTAR_TEST_CASE(sstable_directory_test_table_extra_temporary_toc) {
    return sstables::test_env::do_with_async([] (test_env& env) {
        auto& dir = env.tempdir();
        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir.path(), 1));
        link_file(test::filename(*sst, sstables::component_type::TOC).native(), test::filename(*sst, sstables::component_type::TemporaryTOC).native()).get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            auto expect_ok = distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true });
            BOOST_REQUIRE_NO_THROW(expect_ok.get());
        });
    });
}

// Test the absence of TOC. Behavior is controllable by a flag
SEASTAR_TEST_CASE(sstable_directory_test_table_missing_toc) {
    return sstables::test_env::do_with_async([] (test_env& env) {
        auto& dir = env.tempdir();

        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir.path(), 1));
        remove_file(test::filename(*sst, sstables::component_type::TOC).native()).get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir_fatal) {
            auto expect_malformed_sstable  = distributed_loader_for_tests::process_sstable_dir(sstdir_fatal, { .throw_on_missing_toc = true });
            BOOST_REQUIRE_THROW(expect_malformed_sstable.get(), sstables::malformed_sstable_exception);
        });

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir_ok) {
            auto expect_ok = distributed_loader_for_tests::process_sstable_dir(sstdir_ok, {});
            BOOST_REQUIRE_NO_THROW(expect_ok.get());
        });
    });
}

// Test the presence of TemporaryStatistics. If the old Statistics file is around
// this is benign and we'll just delete it and move on. If the old Statistics file
// is not around (but mentioned in the TOC), then this is an error.
SEASTAR_THREAD_TEST_CASE(sstable_directory_test_temporary_statistics) {
    sstables::test_env::do_with_sharded_async([] (sharded<test_env>& env) {
        auto& dir = env.local().tempdir();

        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env.local()), dir.path(), 1));
        auto tempstr = test::filename(*sst, component_type::TemporaryStatistics);
        auto f = open_file_dma(tempstr.native(), open_flags::rw | open_flags::create | open_flags::truncate).get0();
        f.close().get();
        auto tempstat = fs::canonical(tempstr);

        with_sstable_directory(dir.path(), env, [&dir, &tempstat] (sharded<sstables::sstable_directory>& sstdir_ok) {
            auto expect_ok = distributed_loader_for_tests::process_sstable_dir(sstdir_ok, {});
            BOOST_REQUIRE_NO_THROW(expect_ok.get());
            lister::scan_dir(dir.path(), lister::dir_entry_types::of<directory_entry_type::regular>(), [tempstat] (fs::path parent_dir, directory_entry de) {
                BOOST_REQUIRE(fs::canonical(parent_dir / fs::path(de.name)) != tempstat);
                return make_ready_future<>();
            }).get();
        });

        remove_file(test::filename(*sst, sstables::component_type::Statistics).native()).get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir_fatal) {
            auto expect_malformed_sstable  = distributed_loader_for_tests::process_sstable_dir(sstdir_fatal, {});
            BOOST_REQUIRE_THROW(expect_malformed_sstable.get(), sstables::malformed_sstable_exception);
        });
    }).get();
}

// Test that we see the right generation during the scan. Temporary files are skipped
SEASTAR_THREAD_TEST_CASE(sstable_directory_test_generation_sanity) {
    sstables::test_env::do_with_sharded_async([] (sharded<test_env>& env) {
        auto& dir = env.local().tempdir();
        make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env.local()), dir.path(), 3333));
        auto sst = make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env.local()), dir.path(), 6666));
        rename_file(test::filename(*sst, sstables::component_type::TOC).native(), test::filename(*sst, sstables::component_type::TemporaryTOC).native()).get();

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true }).get();
            int64_t max_generation_seen = highest_generation_seen(sstdir).get0();
            BOOST_REQUIRE_EQUAL(max_generation_seen, 3333);
        });
    }).get();
}

future<> verify_that_all_sstables_are_local(sharded<sstable_directory>& sstdir, unsigned expected_sstables) {
    return do_with(std::make_unique<std::atomic<unsigned>>(0), [&sstdir, expected_sstables] (std::unique_ptr<std::atomic<unsigned>>& count) {
        return sstdir.invoke_on_all([count = count.get()] (sstable_directory& d) {
            return d.do_for_each_sstable([count] (sstables::shared_sstable sst) {
                count->fetch_add(1, std::memory_order_relaxed);
                auto shards = sst->get_shards_for_this_sstable();
                BOOST_REQUIRE_EQUAL(shards.size(), 1);
                BOOST_REQUIRE_EQUAL(shards[0], this_shard_id());
                return make_ready_future<>();
            });
         }).then([count = count.get(), expected_sstables] {
            BOOST_REQUIRE_EQUAL(count->load(std::memory_order_relaxed), expected_sstables);
            return make_ready_future<>();
        });
    });
}

// Test that all SSTables are seen as unshared, if the generation numbers match what their
// shard-assignments expect
SEASTAR_THREAD_TEST_CASE(sstable_directory_unshared_sstables_sanity_matched_generations) {
    sstables::test_env::do_with_sharded_async([] (sharded<test_env>& env) {
        auto& dir = env.local().tempdir();
        for (shard_id i = 0; i < smp::count; ++i) {
            env.invoke_on(i, [dir = dir.path(), i] (sstables::test_env& env) {
                // this is why it is annoying for the internal functions in the test infrastructure to
                // assume threaded execution
                return seastar::async([dir, i, &env] {
                    make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir, i));
                });
            }).get();
        }

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true }).get();
            verify_that_all_sstables_are_local(sstdir, smp::count).get();
        });
    }).get();
}

// Test that all SSTables are seen as unshared, even if the generation numbers do not match what their
// shard-assignments expect
SEASTAR_THREAD_TEST_CASE(sstable_directory_unshared_sstables_sanity_unmatched_generations) {
    sstables::test_env::do_with_sharded_async([] (sharded<test_env>& env) {
        auto& dir = env.local().tempdir();
        for (shard_id i = 0; i < smp::count; ++i) {
            env.invoke_on(i, [dir = dir.path(), i] (sstables::test_env& env) {
                // this is why it is annoying for the internal functions in the test infrastructure to
                // assume threaded execution
                return seastar::async([dir, i, &env] {
                    make_sstable_for_this_shard(std::bind(new_sstable, std::ref(env), dir, i + 1));
                });
            }).get();
        }

        with_sstable_directory(dir.path(), env, [] (sharded<sstables::sstable_directory>& sstdir) {
            distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true }).get();
            verify_that_all_sstables_are_local(sstdir, smp::count).get();
        });
    }).get();
}

// Test that the sstable_dir object can keep the table alive against a drop
SEASTAR_TEST_CASE(sstable_directory_test_table_lock_works) {
    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.execute_cql("create table cf (p text PRIMARY KEY, c int)").get();
        auto ks_name = "ks";
        auto cf_name = "cf";
        auto path = fs::path(e.local_db().find_column_family(ks_name, cf_name).dir());
        std::unordered_map<unsigned, std::vector<sstring>> sstables;

        testlog.debug("Inserting into cf");
        e.execute_cql("insert into cf (p, c) values ('one', 1)").get();

        testlog.debug("Flushing cf");
        e.db().invoke_on_all([&] (replica::database& db) {
            auto& cf = db.find_column_family(ks_name, cf_name);
            return cf.flush();
        }).get();

        with_sstable_directory(path, e, [&] (sharded<sstable_directory>& sstdir) {
            distributed_loader_for_tests::process_sstable_dir(sstdir, {}).get();

            // Collect all sstable file names
            sstdir.invoke_on_all([&] (sstable_directory& d) {
                return d.do_for_each_sstable([&] (sstables::shared_sstable sst) {
                    sstables[this_shard_id()].push_back(test::filename(*sst, sstables::component_type::Data).native());
                    return make_ready_future<>();
                });
            }).get();
            BOOST_REQUIRE(sstables.size() != 0);

            distributed_loader_for_tests::lock_table(sstdir, e.db(), ks_name, cf_name).get();

            auto drop = e.execute_cql("drop table cf");

            auto table_exists = [&] () {
                try {
                    e.db().invoke_on_all([ks_name, cf_name] (replica::database& db) {
                        db.find_column_family(ks_name, cf_name);
                    }).get();
                    return true;
                } catch (replica::no_such_column_family&) {
                    return false;
                }
            };

            testlog.debug("Waiting until {}.{} is unlisted from the database", ks_name, cf_name);
            while (table_exists()) {
                yield().get();
            }

            auto all_sstables_exist = [&] () {
                std::unordered_map<bool, size_t> res;
                for (const auto& [shard, files] : sstables) {
                    for (const auto& f : files) {
                        res[file_exists(f).get0()]++;
                    }
                }
                return res;
            };

            auto res = all_sstables_exist();
            BOOST_REQUIRE(res[false] == 0);
            BOOST_REQUIRE(res[true] == sstables.size());

            // Stop manually now, to allow for the object to be destroyed and take the
            // phaser with it.
            sstdir.stop().get();
            drop.get();

            BOOST_REQUIRE(!table_exists());

            res = all_sstables_exist();
            BOOST_REQUIRE(res[false] == sstables.size());
            BOOST_REQUIRE(res[true] == 0);
        });
    });
}

SEASTAR_TEST_CASE(sstable_directory_shared_sstables_reshard_correctly) {
    if (smp::count == 1) {
        fmt::print("Skipping sstable_directory_shared_sstables_reshard_correctly, smp == 1\n");
        return make_ready_future<>();
    }

    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.execute_cql("create table cf (p text PRIMARY KEY, c int)").get();
        auto& cf = e.local_db().find_column_family("ks", "cf");
        auto upload_path = fs::path(cf.dir()) / sstables::upload_dir;

        e.db().invoke_on_all([] (replica::database& db) {
            auto& cf = db.find_column_family("ks", "cf");
            return cf.disable_auto_compaction();
        }).get();

        unsigned num_sstables = 10 * smp::count;
        auto generation = 0;
        for (unsigned nr = 0; nr < num_sstables; ++nr) {
            make_sstable_for_all_shards(e.db().local(), cf, upload_path.native(), generation++);
        }

      with_sstable_directory(upload_path, e, [&e, upload_path] (sharded<sstables::sstable_directory>& sstdir) {
        distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true }).get();
        verify_that_all_sstables_are_local(sstdir, 0).get();

        int64_t max_generation_seen = highest_generation_seen(sstdir).get0();
        std::atomic<int64_t> generation_for_test = {};
        generation_for_test.store(max_generation_seen + 1, std::memory_order_relaxed);

        distributed_loader_for_tests::reshard(sstdir, e.db(), "ks", "cf", [&e, upload_path, &generation_for_test] (shard_id id) {
            auto generation = generation_for_test.fetch_add(1, std::memory_order_relaxed);
            auto& cf = e.local_db().find_column_family("ks", "cf");
            return cf.get_sstables_manager().make_sstable(cf.schema(), upload_path.native(), generation_from_value(generation), sstables::sstable::version_types::mc, sstables::sstable::format_types::big);
        }).get();
        verify_that_all_sstables_are_local(sstdir, smp::count * smp::count).get();
      });
    });
}

SEASTAR_TEST_CASE(sstable_directory_shared_sstables_reshard_distributes_well_even_if_files_are_not_well_distributed) {
    if (smp::count == 1) {
        fmt::print("Skipping sstable_directory_shared_sstables_reshard_correctly, smp == 1\n");
        return make_ready_future<>();
    }

    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.execute_cql("create table cf (p text PRIMARY KEY, c int)").get();
        auto& cf = e.local_db().find_column_family("ks", "cf");
        auto upload_path = fs::path(cf.dir()) / sstables::upload_dir;

        e.db().invoke_on_all([] (replica::database& db) {
            auto& cf = db.find_column_family("ks", "cf");
            return cf.disable_auto_compaction();
        }).get();

        unsigned num_sstables = 10 * smp::count;
        auto generation = 0;
        for (unsigned nr = 0; nr < num_sstables; ++nr) {
            make_sstable_for_all_shards(e.db().local(), cf, upload_path.native(), generation++ * smp::count);
        }

      with_sstable_directory(upload_path, e, [&e, upload_path] (sharded<sstables::sstable_directory>& sstdir) {
        distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true }).get();
        verify_that_all_sstables_are_local(sstdir, 0).get();

        int64_t max_generation_seen = highest_generation_seen(sstdir).get0();
        std::atomic<int64_t> generation_for_test = {};
        generation_for_test.store(max_generation_seen + 1, std::memory_order_relaxed);

        distributed_loader_for_tests::reshard(sstdir, e.db(), "ks", "cf", [&e, upload_path, &generation_for_test] (shard_id id) {
            auto generation = generation_for_test.fetch_add(1, std::memory_order_relaxed);
            auto& cf = e.local_db().find_column_family("ks", "cf");
            return cf.get_sstables_manager().make_sstable(cf.schema(), upload_path.native(), generation_from_value(generation), sstables::sstable::version_types::mc, sstables::sstable::format_types::big);
        }).get();
        verify_that_all_sstables_are_local(sstdir, smp::count * smp::count).get();
      });
    });
}

SEASTAR_TEST_CASE(sstable_directory_shared_sstables_reshard_respect_max_threshold) {
    if (smp::count == 1) {
        fmt::print("Skipping sstable_directory_shared_sstables_reshard_correctly, smp == 1\n");
        return make_ready_future<>();
    }

    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.execute_cql("create table cf (p text PRIMARY KEY, c int)").get();
        auto& cf = e.local_db().find_column_family("ks", "cf");
        auto upload_path = fs::path(cf.dir()) / sstables::upload_dir;

        e.db().invoke_on_all([] (replica::database& db) {
            auto& cf = db.find_column_family("ks", "cf");
            return cf.disable_auto_compaction();
        }).get();

        unsigned num_sstables = (cf.schema()->max_compaction_threshold() + 1) * smp::count;
        auto generation = 0;
        for (unsigned nr = 0; nr < num_sstables; ++nr) {
            make_sstable_for_all_shards(e.db().local(), cf, upload_path.native(), generation++);
        }

      with_sstable_directory(upload_path, e, [&, upload_path] (sharded<sstables::sstable_directory>& sstdir) {
        distributed_loader_for_tests::process_sstable_dir(sstdir, { .throw_on_missing_toc = true }).get();
        verify_that_all_sstables_are_local(sstdir, 0).get();

        int64_t max_generation_seen = highest_generation_seen(sstdir).get0();
        std::atomic<int64_t> generation_for_test = {};
        generation_for_test.store(max_generation_seen + 1, std::memory_order_relaxed);

        distributed_loader_for_tests::reshard(sstdir, e.db(), "ks", "cf", [&e, upload_path, &generation_for_test] (shard_id id) {
            auto generation = generation_for_test.fetch_add(1, std::memory_order_relaxed);
            auto& cf = e.local_db().find_column_family("ks", "cf");
            return cf.get_sstables_manager().make_sstable(cf.schema(), upload_path.native(), generation_from_value(generation), sstables::sstable::version_types::mc, sstables::sstable::format_types::big);
        }).get();
        verify_that_all_sstables_are_local(sstdir, 2 * smp::count * smp::count).get();
      });
    });
}
