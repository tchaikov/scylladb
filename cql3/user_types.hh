/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Modified by Cloudius Systems
 *
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#include "column_specification.hh"
#include "term.hh"
#include "column_identifier.hh"
#include "constants.hh"
#include "to_string.hh"
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/count.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>

namespace cql3 {

/**
 * Static helper methods and classes for user types.
 */
class user_types {
    user_types() = delete;
public:
    static shared_ptr<column_specification> field_spec_of(shared_ptr<column_specification> column, size_t field) {
        auto&& ut = static_pointer_cast<const user_type_impl>(column->type);
        auto&& name = ut->field_name(field);
        auto&& sname = sstring(reinterpret_cast<const char*>(name.data()), name.size());
        return make_shared<column_specification>(
                                       column->ks_name,
                                       column->cf_name,
                                       make_shared<column_identifier>(column->name->to_string() + "." + sname, true),
                                       ut->field_type(field));
    }

    class literal : public term::raw {
    public:
        using elements_map_type = std::unordered_map<column_identifier, shared_ptr<term::raw>>;
        elements_map_type _entries;

        literal(elements_map_type entries)
                : _entries(std::move(entries)) {
        }

        virtual shared_ptr<term> prepare(database& db, const sstring& keyspace, shared_ptr<column_specification> receiver) override {
            validate_assignable_to(db, keyspace, receiver);
            auto&& ut = static_pointer_cast<const user_type_impl>(receiver->type);
            bool all_terminal = true;
            std::vector<shared_ptr<term>> values;
            values.reserve(_entries.size());
            size_t found_values = 0;
            for (size_t i = 0; i < ut->size(); ++i) {
                auto&& field = column_identifier(to_bytes(ut->field_name(i)), utf8_type);
                auto iraw = _entries.find(field);
                shared_ptr<term::raw> raw;
                if (iraw == _entries.end()) {
                    raw = cql3::constants::NULL_LITERAL;
                } else {
                    raw = iraw->second;
                    ++found_values;
                }
                auto&& value = raw->prepare(db, keyspace, field_spec_of(receiver, i));

                if (dynamic_cast<non_terminal*>(value.get())) {
                    all_terminal = false;
                }

                values.push_back(std::move(value));
            }
            if (found_values != _entries.size()) {
                // We had some field that are not part of the type
                for (auto&& id_val : _entries) {
                    auto&& id = id_val.first;
                    if (!boost::range::count(ut->field_names(), id.bytes_)) {
                        throw exceptions::invalid_request_exception(sprint("Unknown field '%s' in value of user defined type %s", id, ut->get_name_as_string()));
                    }
                }
            }

            delayed_value value(ut, values);
            if (all_terminal) {
                return value.bind(query_options::DEFAULT);
            } else {
                return make_shared(std::move(value));
            }
        }
    private:
        void validate_assignable_to(database& db, const sstring& keyspace, shared_ptr<column_specification> receiver) {
            auto&& ut = dynamic_pointer_cast<const user_type_impl>(receiver->type);
            if (!ut) {
                throw exceptions::invalid_request_exception(sprint("Invalid user type literal for %s of type %s", receiver->name, receiver->type->as_cql3_type()));
            }

            for (size_t i = 0; i < ut->size(); i++) {
                column_identifier field(to_bytes(ut->field_name(i)), utf8_type);
                if (_entries.count(field) == 0) {
                    continue;
                }
                shared_ptr<term::raw> value = _entries[field];
                auto&& field_spec = field_spec_of(receiver, i);
                if (!assignment_testable::is_assignable(value->test_assignment(db, keyspace, field_spec))) {
                    throw exceptions::invalid_request_exception(sprint("Invalid user type literal for %s: field %s is not of type %s", receiver->name, field, field_spec->type->as_cql3_type()));
                }
            }
        }
    public:
        virtual assignment_testable::test_result test_assignment(database& db, const sstring& keyspace, shared_ptr<column_specification> receiver) override {
            try {
                validate_assignable_to(db, keyspace, receiver);
                return assignment_testable::test_result::WEAKLY_ASSIGNABLE;
            } catch (exceptions::invalid_request_exception& e) {
                return assignment_testable::test_result::NOT_ASSIGNABLE;
            }
        }

        virtual sstring assignment_testable_source_context() const override {
            return to_string();
        }

        virtual sstring to_string() const override {
            auto kv_to_str = [] (auto&& kv) { return sprint("%s:%s", kv.first, kv.second); };
            return sprint("{%s}", ::join(", ", _entries | boost::adaptors::transformed(kv_to_str)));
        }
    };

    // Same purpose than Lists.DelayedValue, except we do handle bind marker in that case
    class delayed_value : public non_terminal {
        user_type _type;
        std::vector<shared_ptr<term>> _values;
    public:
        delayed_value(user_type type, std::vector<shared_ptr<term>> values)
                : _type(std::move(type)), _values(std::move(values)) {
        }
        virtual bool uses_function(const sstring& ks_name, const sstring& function_name) const override {
            return boost::algorithm::any_of(_values,
                        std::bind(&term::uses_function, std::placeholders::_1, std::cref(ks_name), std::cref(function_name)));
        }
        virtual bool contains_bind_marker() const override {
            return boost::algorithm::any_of(_values, std::mem_fn(&term::contains_bind_marker));
        }

        virtual void collect_marker_specification(shared_ptr<variable_specifications> bound_names) {
            for (auto&& v : _values) {
                v->collect_marker_specification(bound_names);
            }
        }
    private:
        std::vector<bytes_opt> bind_internal(const query_options& options) {
            auto sf = options.get_serialization_format();
            std::vector<bytes_opt> buffers;
            for (size_t i = 0; i < _type->size(); ++i) {
                buffers.push_back(_values[i]->bind_and_get(options));
                // Inside UDT values, we must force the serialization of collections to v3 whatever protocol
                // version is in use since we're going to store directly that serialized value.
                if (sf != serialization_format::use_32_bit() && _type->field_type(i)->is_collection() && buffers.back()) {
                    auto&& ctype = static_pointer_cast<const collection_type_impl>(_type->field_type(i));
                    buffers.back() = ctype->reserialize(sf, serialization_format::use_32_bit(), bytes_view(*buffers.back()));
                }
            }
            return buffers;
        }
    public:
        virtual shared_ptr<terminal> bind(const query_options& options) override {
            return ::make_shared<constants::value>(bind_and_get(options));
        }

        virtual bytes_opt bind_and_get(const query_options& options) override {
            return user_type_impl::build_value(bind_internal(options));
        }
    };
};

}
