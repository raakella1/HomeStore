#ifndef _HOMESTORE_HEADER_HPP_
#define _HOMESTORE_HEADER_HPP_

#include <boost/uuid/uuid.hpp>
#include <string>

#ifdef _PRERELEASE
#include <flip/flip.hpp>
#endif

namespace homeds {
struct blob {
    uint8_t* bytes;
    uint32_t size;
};
} // namespace homeds

namespace homestore {

enum io_flag {
    BUFFERED_IO = 0, // should be set if file system doesn't support direct IOs and we are working on a file as a disk.
                     // This option is enabled only on in debug build.
    DIRECT_IO = 1,  // recommened mode
    READ_ONLY = 2   // Read-only mode for post-mortem checks
};

struct dev_info {
    std::string dev_names;
};

#ifdef _PRERELEASE
class HomeStoreFlip {
public:
    static flip::Flip* instance() {
        static flip::Flip inst;
        return &(flip::Flip::instance());
    }

    static flip::FlipClient* client_instance() {
        static flip::FlipClient fc(HomeStoreFlip::instance());
        return &fc;
    }
};

#define homestore_flip (&flip::Flip::instance())
#endif

#define METRICS_DUMP_MSG sisl::MetricsFarm::getInstance().get_result_in_json_string()

#ifndef NDEBUG
#define DEBUG_METRICS_DUMP_FORMAT METRICS_DUMP_FORMAT
#define DEBUG_METRICS_DUMP_MSG METRICS_DUMP_MSG

#define LOGMSG_METRICS_DUMP_FORMAT METRICS_DUMP_FORMAT
#define LOGMSG_METRICS_DUMP_MSG METRICS_DUMP_MSG

#define RELEASE_METRICS_DUMP_FORMAT METRICS_DUMP_FORMAT
#define RELEASE_METRICS_DUMP_MSG METRICS_DUMP_MSG
#else
#define DEBUG_METRICS_DUMP_FORMAT "{}"
#define DEBUG_METRICS_DUMP_MSG "N/A"

#define LOGMSG_METRICS_DUMP_FORMAT "{}"
#define LOGMSG_METRICS_DUMP_MSG "N/A"

#define RELEASE_METRICS_DUMP_FORMAT METRICS_DUMP_FORMAT
#define RELEASE_METRICS_DUMP_MSG METRICS_DUMP_MSG
#endif

#if 0
#define HS_LOG(buf, level, mod, req, f, ...)                                                                           \
    BOOST_PP_IF(BOOST_PP_IS_EMPTY(req), , fmt::format_to(_log_buf, "[req_id={}] ", req->request_id));                  \
    fmt::format_to(_log_buf, f, ##__VA_ARGS__);                                                                        \
    fmt::format_to(_log_buf, "{}", (char)0);                                                                           \
    LOG##level##MOD(BOOST_PP_IF(BOOST_PP_IS_EMPTY(mod), base, mod), "{}", _log_buf.data());
#endif

#define HOMESTORE_LOG_MODS                                                                                             \
    btree_structures, btree_nodes, btree_generics, cache, device, httpserver_lmod, iomgr, varsize_blk_alloc,           \
        VMOD_VOL_MAPPING, volume, flip, cp

template< typename T >
std::string to_hex( T i ) {
    return fmt::format("{0:x}", i);
}


} // namespace homestore
#endif
