#include <boost/test/unit_test.hpp>
#include <memory>
#include <utility>

#include <seastar/core/sstring.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/distributed.hh>
#include "types/map.hh"
#include "sstables/sstables.hh"
#include "test/lib/scylla_test_case.hh"
#include "schema/schema.hh"
#include "replica/database.hh"
#include "dht/murmur3_partitioner.hh"
#include "compaction/compaction_manager.hh"
#include "test/boost/sstable_test.hh"
#include "test/lib/tmpdir.hh"
#include "cell_locking.hh"
#include "test/lib/flat_mutation_reader_assertions.hh"
#include "test/lib/key_utils.hh"
#include "test/lib/sstable_utils.hh"
#include "test/lib/test_services.hh"
#include "service/storage_proxy.hh"
#include "db/config.hh"

using namespace sstables;

static schema_builder get_schema_builder() {
    return schema_builder("tests", "sstable_resharding_test")
        .with_column("id", utf8_type, column_kind::partition_key)
        .with_column("value", int32_type);
}

static schema_ptr get_schema() {
    return get_schema_builder().build();
}

static schema_ptr get_schema(unsigned shard_count, unsigned sharding_ignore_msb_bits) {
    return get_schema_builder().with_sharder(shard_count, sharding_ignore_msb_bits).build();
}

// Asserts that sstable::compute_owner_shards(...) produces correct results.
static future<> assert_sstable_computes_correct_owners(test_env& env, const sstables::shared_sstable& base_sst, sstring dir) {
    auto sst = env.make_sstable(base_sst->get_schema(), dir, int64_t(base_sst->generation()), base_sst->get_version());
    co_await sst->load_owner_shards();
    BOOST_REQUIRE_EQUAL(sst->get_shards_for_this_sstable(), base_sst->get_shards_for_this_sstable());
}

void run_sstable_resharding_test() {
    test_env env;
    auto close_env = defer([&] { env.stop().get(); });
  for (const auto version : writable_sstable_versions) {
    auto tmp = tmpdir();
    auto s = get_schema();
    table_for_tests cf(env.manager(), s);
    auto close_cf = deferred_stop(cf);
    std::unordered_map<shard_id, std::vector<mutation>> muts;
    static constexpr auto keys_per_shard = 1000u;

    // create sst shared by all shards
    {
        auto mt = make_lw_shared<replica::memtable>(s);
        auto get_mutation = [mt, s] (const dht::decorated_key& key, auto value) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(value)), api::timestamp_type(0));
            return m;
        };
        auto cfg = std::make_unique<db::config>();
        for (auto i : boost::irange(0u, smp::count)) {
            const auto keys = tests::generate_partition_keys(keys_per_shard, s, i);
            BOOST_REQUIRE(keys.size() == keys_per_shard);
            muts[i].reserve(keys_per_shard);
            for (auto k : boost::irange(0u, keys_per_shard)) {
                auto m = get_mutation(keys[k], i);
                muts[i].push_back(m);
                mt->apply(std::move(m));
            }
        }
        auto sst = env.make_sstable(s, tmp.path().string(), 0, version, sstables::sstable::format_types::big);
        write_memtable_to_sstable_for_test(*mt, sst).get();
    }
    auto sst = env.reusable_sst(s, tmp.path().string(), 0, version, sstables::sstable::format_types::big).get0();

    // FIXME: sstable write has a limitation in which it will generate sharding metadata only
    // for a single shard. workaround that by setting shards manually. from this test perspective,
    // it doesn't matter because we check each partition individually of each sstable created
    // for a shard that owns the shared input sstable.
    sstables::test(sst).set_shards(boost::copy_range<std::vector<unsigned>>(boost::irange(0u, smp::count)));

    auto filter_fname = sstables::test(sst).filename(component_type::Filter);
    uint64_t bloom_filter_size_before = file_size(filter_fname).get0();

    auto descriptor = sstables::compaction_descriptor({sst}, default_priority_class(), 0, std::numeric_limits<uint64_t>::max());
    descriptor.options = sstables::compaction_type_options::make_reshard();
    descriptor.creator = [&env, &cf, &tmp, version] (shard_id shard) mutable {
        // we need generation calculated by instance of cf at requested shard,
        // or resource usage wouldn't be fairly distributed among shards.
        auto gen = smp::submit_to(shard, [&cf] () {
            return column_family_test::calculate_generation_for_new_table(*cf);
        }).get0();

        return env.make_sstable(cf->schema(), tmp.path().string(), gen,
            version, sstables::sstable::format_types::big);
    };
    auto cdata = compaction_manager::create_compaction_data();
    auto res = sstables::compact_sstables(std::move(descriptor), cdata, cf.as_table_state()).get0();
    auto new_sstables = std::move(res.new_sstables);
    BOOST_REQUIRE(new_sstables.size() == smp::count);

    uint64_t bloom_filter_size_after = 0;
    std::unordered_set<shard_id> processed_shards;

    for (auto& sstable : new_sstables) {
        auto new_sst = env.reusable_sst(s, tmp.path().string(), int64_t(sstable->generation()),
            version, sstables::sstable::format_types::big).get0();
        filter_fname = sstables::test(new_sst).filename(component_type::Filter);
        bloom_filter_size_after += file_size(filter_fname).get0();
        auto shards = new_sst->get_shards_for_this_sstable();
        BOOST_REQUIRE(shards.size() == 1); // check sstable is unshared.
        auto shard = shards.front();
        BOOST_REQUIRE(processed_shards.insert(shard).second == true); // check resharding created one sstable per shard.
        assert_sstable_computes_correct_owners(env, new_sst, tmp.path().string()).get();

        auto rd = assert_that(new_sst->as_mutation_source().make_reader_v2(s, env.make_reader_permit()));
        BOOST_REQUIRE(muts[shard].size() == keys_per_shard);
        for (auto k : boost::irange(0u, keys_per_shard)) {
            rd.produces(muts[shard][k]);
        }
        rd.produces_end_of_stream();
    }
    BOOST_REQUIRE_CLOSE_FRACTION(float(bloom_filter_size_before), float(bloom_filter_size_after), 0.1);
  }
}

SEASTAR_TEST_CASE(sstable_resharding_test) {
    return seastar::async([] {
        run_sstable_resharding_test();
    });
}

SEASTAR_TEST_CASE(sstable_is_shared_correctness) {
    return test_env::do_with_async([] (test_env& env) {
      for (const auto version : writable_sstable_versions) {
        auto tmp = tmpdir();
        auto cfg = std::make_unique<db::config>();

        auto get_mutation = [] (const schema_ptr& s, const dht::decorated_key& key, auto value) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(value)), api::timestamp_type(0));
            return m;
        };
        auto gen = make_lw_shared<unsigned>(1);

        // created sstable owned only by this shard
        {
            auto s = get_schema();
            auto sst_gen = [&env, s, &tmp, gen, version]() mutable {
                return env.make_sstable(s, tmp.path().string(), (*gen)++, version, big);
            };

            const auto keys = tests::generate_partition_keys(smp::count * 10, s);
            std::vector<mutation> muts;
            for (auto& k : keys) {
                muts.push_back(get_mutation(s, k, 0));
            }

            auto sst = make_sstable_containing(sst_gen, muts);
            BOOST_REQUIRE(!sst->is_shared());
            assert_sstable_computes_correct_owners(env, sst, tmp.path().string()).get();
        }

        // create sstable owned by all shards
        // created unshared sstable
        {
            auto key_s = get_schema();
            auto single_sharded_s = get_schema(1, cfg->murmur3_partitioner_ignore_msb_bits());
            auto sst_gen = [&env, single_sharded_s, &tmp, gen, version]() mutable {
                return env.make_sstable(single_sharded_s, tmp.path().string(), (*gen)++, version, big);
            };

            std::vector<mutation> muts;
            for (shard_id shard : boost::irange(0u, smp::count)) {
                const auto keys = tests::generate_partition_keys(10, key_s, shard);
                for (auto& k : keys) {
                    muts.push_back(get_mutation(single_sharded_s, k, shard));
                }
            }

            auto sst = make_sstable_containing(sst_gen, muts);
            BOOST_REQUIRE(!sst->is_shared());

            auto all_shards_s = get_schema(smp::count, cfg->murmur3_partitioner_ignore_msb_bits());
            sst = env.reusable_sst(all_shards_s, tmp.path().string(), int64_t(sst->generation()), version).get0();
            BOOST_REQUIRE(smp::count == 1 || sst->is_shared());
            BOOST_REQUIRE(sst->get_shards_for_this_sstable().size() == smp::count);
            assert_sstable_computes_correct_owners(env, sst, tmp.path().string()).get();
        }
      }
    });
}
