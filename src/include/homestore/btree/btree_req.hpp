/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once
#include <sisl/fds/buffer.hpp>
#include <homestore/btree/btree_kv.hpp>

namespace homestore {
struct BtreeRequest;

typedef std::pair< BtreeKey, BtreeValue > btree_kv_t;

// Base class for any btree operations
struct BtreeRequest {
    BtreeRequest() = default;
    BtreeRequest(void* app_ctx, void* op_ctx) : m_app_context{app_ctx}, m_op_context{op_ctx} {}

    void enable_route_tracing() {
        route_tracing = std::make_unique< std::vector< trace_route_entry > >();
        route_tracing->reserve(8);
    }

    std::string route_string() const {
        std::string out;
        if (route_tracing) {
            fmt::format_to(std::back_inserter(out), "Route size={}\n", route_tracing->size());
            for (const auto& r : *route_tracing) {
                fmt::format_to(std::back_inserter(out), "{}\n", r.to_string());
            }
        }
        return out;
    }

    void* m_app_context{nullptr};
    void* m_op_context{nullptr};
    std::unique_ptr< std::vector< trace_route_entry > > route_tracing{nullptr};
};

// Base class for all range related operations
template < typename K >
struct BtreeRangeRequest : public BtreeRequest {
public:
    uint32_t batch_size() const { return m_batch_size; }
    void set_batch_size(uint32_t count) { m_batch_size = count; }

    bool is_empty_cursor() const {
        return ((m_search_state.const_cursor()->m_last_key == nullptr) &&
                (m_search_state.const_cursor()->m_locked_nodes == nullptr));
    }

    BtreeTraversalState< K >& search_state() { return m_search_state; }
    BtreeQueryCursor< K >* cursor() { return m_search_state.cursor(); }
    const BtreeQueryCursor< K >* const_cursor() const { return m_search_state.const_cursor(); }

    const BtreeKeyRange< K >& input_range() const { return m_search_state.input_range(); }
    const BtreeKeyRange< K >& next_range() { return m_search_state.next_range(); }
    const BtreeKeyRange< K >& working_range() const { return m_search_state.working_range(); }

    const K& next_key() const { return m_search_state.next_key(); }
    void trim_working_range(K&& end_key, bool end_incl) {
        m_search_state.trim_working_range(std::move(end_key), end_incl);
    }
    void set_cursor_key(const K& end_key) { return m_search_state.set_cursor_key(end_key); }

protected:
    BtreeRangeRequest(BtreeKeyRange< K >&& input_range, bool external_pagination = false, void* app_context = nullptr,
                      uint32_t batch_size = UINT32_MAX) :
            BtreeRequest{app_context, nullptr},
            m_search_state{std::move(input_range), external_pagination},
            m_batch_size{batch_size} {}

private:
    BtreeTraversalState< K > m_search_state;
    uint32_t m_batch_size{1};
};

/////////////////////////// 1: Put Operations /////////////////////////////////////
struct BtreeSinglePutRequest : public BtreeRequest {
public:
    BtreeSinglePutRequest(const BtreeKey* k, const BtreeValue* v, btree_put_type put_type,
                          BtreeValue* existing_val = nullptr) :
            m_k{k}, m_v{v}, m_put_type{put_type}, m_existing_val{existing_val} {}

    const BtreeKey& key() const { return *m_k; }
    const BtreeValue& value() const { return *m_v; }

    const BtreeKey* m_k;
    const BtreeValue* m_v;
    const btree_put_type m_put_type;
    BtreeValue* m_existing_val;
};

template < typename K >
struct BtreeRangePutRequest : public BtreeRangeRequest< K > {
public:
    BtreeRangePutRequest(BtreeKeyRange< K >&& inp_range, btree_put_type put_type, const BtreeValue* value,
                         void* app_context = nullptr, uint32_t batch_size = std::numeric_limits< uint32_t >::max()) :
            BtreeRangeRequest< K >(std::move(inp_range), false, app_context, batch_size),
            m_put_type{put_type},
            m_newval{value} {}

    const btree_put_type m_put_type{btree_put_type::REPLACE_ONLY_IF_EXISTS};
    const BtreeValue* m_newval;
};

/////////////////////////// 2: Remove Operations /////////////////////////////////////
struct BtreeSingleRemoveRequest : public BtreeRequest {
public:
    BtreeSingleRemoveRequest(const BtreeKey* k, BtreeValue* out_val) : m_k{k}, m_outval{out_val} {}

    const BtreeKey& key() const { return *m_k; }
    const BtreeValue& value() const { return *m_outval; }

    const BtreeKey* m_k;
    BtreeValue* m_outval;
};

template < typename K >
struct BtreeRemoveAnyRequest : public BtreeRequest {
public:
    BtreeRemoveAnyRequest(BtreeKeyRange< K >&& inp_range, BtreeKey* out_key, BtreeValue* out_val) :
            m_range{std::move(inp_range)}, m_outkey{out_key}, m_outval{out_val} {}

    BtreeKeyRange< K > m_range;
    BtreeKey* m_outkey;
    BtreeValue* m_outval;
};

template < typename K >
struct BtreeRangeRemoveRequest : public BtreeRangeRequest< K > {
public:
    BtreeRangeRemoveRequest(BtreeKeyRange< K >&& inp_range, void* app_context = nullptr,
                            uint32_t batch_size = std::numeric_limits< uint32_t >::max()) :
            BtreeRangeRequest< K >(std::move(inp_range), false, app_context, batch_size) {}
};

/////////////////////////// 3: Get Operations /////////////////////////////////////
struct BtreeSingleGetRequest : public BtreeRequest {
public:
    BtreeSingleGetRequest(const BtreeKey* k, BtreeValue* out_val) : m_k{k}, m_outval{out_val} {}

    const BtreeKey& key() const { return *m_k; }
    const BtreeValue& value() const { return *m_outval; }

    const BtreeKey* m_k;
    BtreeValue* m_outval;
};

template < typename K >
struct BtreeGetAnyRequest : public BtreeRequest {
public:
    BtreeGetAnyRequest(BtreeKeyRange< K >&& range, BtreeKey* out_key, BtreeValue* out_val) :
            m_range{std::move(range)}, m_outkey{out_key}, m_outval{out_val} {}

    BtreeKeyRange< K > m_range;
    BtreeKey* m_outkey;
    BtreeValue* m_outval;
};

/////////////////////////// 4 Range Query Operations /////////////////////////////////////
ENUM(BtreeQueryType, uint8_t,
     // This is default query which walks to first element in range, and then sweeps/walks
     // across the leaf nodes. However, if upon pagination, it again walks down the query from
     // the key it left off.
     SWEEP_NON_INTRUSIVE_PAGINATION_QUERY,

     // Similar to sweep query, except that it retains the node and its lock during
     // pagination. This is more of intrusive query and if the caller is not careful, the read
     // lock will never be unlocked and could cause deadlocks. Use this option carefully.
     SWEEP_INTRUSIVE_PAGINATION_QUERY,

     // This is relatively inefficient query where every leaf node goes from its parent node
     // instead of walking the leaf node across. This is useful only if we want to check and
     // recover if parent and leaf node are in different generations or crash recovery cases.
     TREE_TRAVERSAL_QUERY,

     // This is both inefficient and quiet intrusive/unsafe query, where it locks the range
     // that is being queried for and do not allow any insert or update within that range. It
     // essentially create a serializable level of isolation.
     SERIALIZABLE_QUERY)

template < typename K >
struct BtreeQueryRequest : public BtreeRangeRequest< K > {
public:
    BtreeQueryRequest(BtreeKeyRange< K >&& inp_range,
                      BtreeQueryType query_type = BtreeQueryType::SWEEP_NON_INTRUSIVE_PAGINATION_QUERY,
                      uint32_t batch_size = UINT32_MAX, void* app_context = nullptr) :
            BtreeRangeRequest< K >{std::move(inp_range), true, app_context, batch_size}, m_query_type{query_type} {}
    ~BtreeQueryRequest() = default;

    // virtual bool is_serializable() const = 0;
    BtreeQueryType query_type() const { return m_query_type; }

protected:
    const BtreeQueryType m_query_type;                                // Type of the query
    const std::unique_ptr< BtreeQueryCursor< K > > m_paginated_query; // Is it a paginated query
};

/* This class is a top level class to keep track of the locks that are held currently. It is
 * used for serializabke query to unlock all nodes in right order at the end of the lock */
class BtreeLockTracker {
public:
    virtual ~BtreeLockTracker() = default;
};

#if 0
class BtreeSweepQueryRequest : public BtreeQueryRequest {
public:
    BtreeSweepQueryRequest(const BtreeSearchRange& criteria, uint32_t iter_count = 1000,
            const match_item_cb_t& match_item_cb = nullptr) :
            BtreeQueryRequest(criteria, iter_count, match_item_cb) {}

    BtreeSweepQueryRequest(const BtreeSearchRange &criteria, const match_item_cb_t& match_item_cb) :
            BtreeQueryRequest(criteria, 1000, match_item_cb) {}

    bool is_serializable() const { return false; }
};

class BtreeSerializableQueryRequest : public BtreeQueryRequest {
public:
    BtreeSerializableQueryRequest(const BtreeSearchRange &range, uint32_t iter_count = 1000,
                             const match_item_cb_t& match_item_cb = nullptr) :
            BtreeQueryRequest(range, iter_count, match_item_cb) {}

    BtreeSerializableQueryRequest(const BtreeSearchRange &criteria, const match_item_cb_t& match_item_cb) :
            BtreeSerializableQueryRequest(criteria, 1000, match_item_cb) {}

    bool is_serializable() const { return true; }
};
#endif
} // namespace homestore
