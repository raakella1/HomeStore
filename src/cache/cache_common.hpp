//
// Created by Kadayam, Hari on 22/04/18.
//
#include "homeds/utility/stats.hpp"
#include <sds_logging/logging.h>

#pragma once

SDS_LOGGING_DECL(cache_vmod_evict, cache_vmod_read, cache_vmod_write)

using namespace homeds;

namespace homestore {
#define CVALUES              \
    X(CACHE_STATS_OBJ_COUNT,            COUNTER,    STATS_INVALID_INDEX, "Cache Object Count")    \
    X(CACHE_STATS_HIT_COUNT,            COUNTER,    STATS_INVALID_INDEX, "Cache hit Count") \
    X(CACHE_STATS_MISS_COUNT,           COUNTER,    STATS_INVALID_INDEX, "Cache miss Count") \
    X(CACHE_STATS_EVICT_COUNT,          COUNTER,    STATS_INVALID_INDEX, "Cache evict Count") \
    X(CACHE_STATS_FAILED_EVICT_COUNT,   COUNTER,    STATS_INVALID_INDEX, "Cache unable to evict countt")

#define X(ind, type, mean_of, desc) ind,
enum cache_stats_type : uint32_t {
    CVALUES
};
#undef X

#define X(ind, type, mean_of, desc) {ind, type, mean_of, desc},
static std::vector< homeds::stats_key > cache_stats_keys = {CVALUES};
#undef X

class CacheStats : public homeds::Stats {
public:
    CacheStats() :
    //Stats({CVALUES}) {}
            Stats(cache_stats_keys) {}

    int get_hit_ratio() const {
        return (get_hit_count() * 100)/(get_hit_count() + get_miss_count());
    }

    uint64_t get_hit_count() const {
        return this->get(CACHE_STATS_HIT_COUNT);
    }

    uint64_t get_miss_count() const {
        return this->get(CACHE_STATS_MISS_COUNT);
    }

    uint64_t get_evict_count() const {
        return this->get(CACHE_STATS_EVICT_COUNT);
    }

    uint64_t get_failed_evict_count() const {
        return this->get(CACHE_STATS_FAILED_EVICT_COUNT);
    }
};
} // namespace homestore
