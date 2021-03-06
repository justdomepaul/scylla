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
 * Copyright (C) 2015 ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "secondary_index.hh"
#include "index/target_parser.hh"
#include "cql3/statements/index_target.hh"

#include <regex>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptors.hpp>

#include "json.hh"

const sstring db::index::secondary_index::custom_index_option_name = "class_name";
const sstring db::index::secondary_index::index_keys_option_name = "index_keys";
const sstring db::index::secondary_index::index_values_option_name = "index_values";
const sstring db::index::secondary_index::index_entries_option_name = "index_keys_and_values";

namespace secondary_index {

static constexpr auto PK_TARGET_KEY = "pk";
static constexpr auto CK_TARGET_KEY = "ck";

static const std::regex target_regex("^(keys|entries|values|full)\\((.+)\\)$");

target_parser::target_info target_parser::parse(schema_ptr schema, const index_metadata& im) {
    sstring target = im.options().at(cql3::statements::index_target::target_option_name);
    try {
        return parse(schema, target);
    } catch (...) {
        throw exceptions::configuration_exception(format("Unable to parse targets for index {} ({}): {}", im.name(), target, std::current_exception()));
    }
}

target_parser::target_info target_parser::parse(schema_ptr schema, const sstring& target) {
    using namespace cql3::statements;
    target_info info;

    auto get_column = [&schema] (const sstring& name) -> const column_definition* {
        const column_definition* cdef = schema->get_column_definition(utf8_type->decompose(name));
        if (!cdef) {
            throw std::runtime_error(format("Column {} not found", name));
        }
        return cdef;
    };

    std::cmatch match;
    if (std::regex_match(target.data(), match, target_regex)) {
        info.type = index_target::from_sstring(match[1].str());
        info.pk_columns.push_back(get_column(sstring(match[2].str())));
        return info;
    }

    Json::Value json_value;
    const bool is_json = json::to_json_value(target, json_value);
    if (is_json && json_value.isObject()) {
        Json::Value pk = json_value.get(PK_TARGET_KEY, Json::Value(Json::arrayValue));
        Json::Value ck = json_value.get(CK_TARGET_KEY, Json::Value(Json::arrayValue));
        if (!pk.isArray() || !ck.isArray()) {
            throw std::runtime_error("pk and ck fields of JSON definition must be arrays");
        }
        for (auto it = pk.begin(); it != pk.end(); ++it) {
            info.pk_columns.push_back(get_column(sstring(it->asString())));
        }
        for (auto it = ck.begin(); it != ck.end(); ++it) {
            info.ck_columns.push_back(get_column(sstring(it->asString())));
        }
        info.type = index_target::target_type::values;
        return info;
    }

    // Fallback and treat the whole string as a single target
    return target_info{{get_column(target)}, {}, index_target::target_type::values};
}

bool target_parser::is_local(sstring target_string) {
    Json::Value json_value;
    const bool is_json = json::to_json_value(target_string, json_value);
    if (!is_json) {
        return false;
    }
    Json::Value pk = json_value.get(PK_TARGET_KEY, Json::Value(Json::arrayValue));
    Json::Value ck = json_value.get(CK_TARGET_KEY, Json::Value(Json::arrayValue));
    return !pk.empty() && !ck.empty();
}

sstring target_parser::get_target_column_name_from_string(const sstring& targets) {
    Json::Value json_value;
    const bool is_json = json::to_json_value(targets, json_value);

    if (!is_json) {
        return targets;
    }

    Json::Value pk = json_value.get("pk", Json::Value(Json::arrayValue));
    Json::Value ck = json_value.get("ck", Json::Value(Json::arrayValue));
    if (ck.isArray() && !ck.empty()) {
        return ck[0].asString();
    }
    if (pk.isArray() && !pk.empty()) {
        return pk[0].asString();
    }
    return targets;
}

sstring target_parser::serialize_targets(const std::vector<::shared_ptr<cql3::statements::index_target>>& targets) {
    using cql3::statements::index_target;

    struct as_json_visitor {
        Json::Value operator()(const index_target::multiple_columns& columns) const {
            Json::Value json_array(Json::arrayValue);
            for (const auto& column : columns) {
                json_array.append(Json::Value(column->to_string()));
            }
            return json_array;
        }

        Json::Value operator()(const index_target::single_column& column) const {
            return Json::Value(column->to_string());
        }
    };

    if (targets.size() == 1 && std::holds_alternative<index_target::single_column>(targets.front()->value)) {
        return std::get<index_target::single_column>(targets.front()->value)->to_string();
    }

    Json::Value json_map(Json::objectValue);
    Json::Value pk_json = std::visit(as_json_visitor(), targets.front()->value);
    if (!pk_json.isArray()) {
        Json::Value pk_array(Json::arrayValue);
        pk_array.append(std::move(pk_json));
        pk_json = std::move(pk_array);
    }
    json_map[PK_TARGET_KEY] = std::move(pk_json);
    if (targets.size() > 1) {
        Json::Value ck_json(Json::arrayValue);
        for (unsigned i = 1; i < targets.size(); ++i) {
            ck_json.append(std::visit(as_json_visitor(), targets.at(i)->value));
        }
        json_map[CK_TARGET_KEY] = ck_json;
    }
    return json::to_sstring(json_map);
}

}
