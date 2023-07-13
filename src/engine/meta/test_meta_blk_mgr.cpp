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
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <iomgr/aio_drive_interface.hpp>
#include <iomgr/io_environment.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include <gtest/gtest.h>

#include "api/meta_interface.hpp"
#include "meta_sb.hpp"
#include "homeblks/home_blks.hpp"
#include "test_common/bits_generator.hpp"
#include "test_common/homestore_test_common.hpp"

using namespace homestore;

RCU_REGISTER_INIT
SISL_LOGGING_INIT(HOMESTORE_LOG_MODS)

SISL_OPTIONS_ENABLE(logging, test_meta_blk_mgr)

SISL_LOGGING_DECL(test_meta_blk_mgr)

struct Param {
    uint64_t num_io;
    uint64_t run_time;
    uint32_t per_write;
    uint32_t num_threads;
    uint32_t per_update;
    uint32_t per_remove;
    bool fixed_wrt_sz_enabled;
    uint32_t fixed_wrt_sz;
    uint32_t min_wrt_sz;
    uint32_t max_wrt_sz;
    bool always_do_overflow;
    bool is_spdk;
    bool is_bitmap;
    std::vector< std::string > dev_names;
};

static Param gp;

static const std::string META_FILE_PREFIX{"/tmp/test_meta_blk_mgr_"};
static constexpr uint64_t Ki{1024};
static constexpr uint64_t Mi{Ki * Ki};
static constexpr uint64_t Gi{Ki * Mi};

static void start_homestore(const uint32_t ndevices, const uint64_t dev_size, const uint32_t nthreads) {
    std::vector< dev_info > device_info;
    // these should be static so that they stay in scope in the lambda in case function ends before lambda completes
    static std::mutex start_mutex;
    static std::condition_variable cv;
    static bool inited;

    inited = false;

    if (gp.dev_names.size()) {
        /* if user customized file/disk names */
        for (uint32_t i{0}; i < gp.dev_names.size(); ++i) {
            const std::filesystem::path fpath{gp.dev_names[i]};
            device_info.emplace_back(gp.dev_names[i], HSDevType::Data);
        }
    } else {
        /* create files */
        LOGINFO("creating {} device files with each of size {} ", ndevices, dev_size);
        for (uint32_t i{0}; i < ndevices; ++i) {
            const std::filesystem::path fpath{META_FILE_PREFIX + std::to_string(i + 1)};
            std::ofstream ofs{fpath.string(), std::ios::binary | std::ios::out};
            std::filesystem::resize_file(fpath, dev_size); // set the file size
            device_info.emplace_back(std::filesystem::canonical(fpath).string(), HSDevType::Data);
        }
    }
    LOGINFO("Starting iomgr with {} threads", nthreads);
    ioenvironment.with_iomgr(nthreads, gp.is_spdk);

    const uint64_t app_mem_size{((ndevices * dev_size) * 15) / 100};
    LOGINFO("Initialize and start HomeBlks with app_mem_size = {}", app_mem_size);

    boost::uuids::string_generator gen;
    init_params params;
    params.data_open_flags = homestore::io_flag::DIRECT_IO;
    params.min_virtual_page_size = 4096;
    params.app_mem_size = app_mem_size;
    params.data_devices = device_info;
    params.init_done_cb = [&tl_start_mutex = start_mutex, &tl_cv = cv, &tl_inited = inited](std::error_condition err,
                                                                                            const out_params& params) {
        LOGINFO("HomeBlks Init completed");
        {
            std::unique_lock< std::mutex > lk{tl_start_mutex};
            tl_inited = true;
        }
        tl_cv.notify_one();
    };
    params.vol_mounted_cb = [](const VolumePtr& vol_obj, vol_state state) {};
    params.vol_state_change_cb = [](const VolumePtr& vol, vol_state old_state, vol_state new_state) {};
    params.vol_found_cb = [](boost::uuids::uuid uuid) -> bool { return true; };

    if (SISL_OPTIONS.count("http_port")) {
        test_common::set_fixed_http_port(SISL_OPTIONS["http_port"].as< uint32_t >());
    } else {
        test_common::set_random_http_port();
    }
    VolInterface::init(params);

    {
        std::unique_lock< std::mutex > lk{start_mutex};
        cv.wait(lk, [] { return inited; });
    }
}

struct sb_info_t {
    void* cookie;
    std::string str;
};

class VMetaBlkMgrTest : public ::testing::Test {
public:
    enum class meta_op_type : uint8_t { write = 1, update = 2, remove = 3, read = 4 };
    std::string mtype;

    VMetaBlkMgrTest() = default;
    VMetaBlkMgrTest(const VMetaBlkMgrTest&) = delete;
    VMetaBlkMgrTest& operator=(const VMetaBlkMgrTest&) = delete;
    VMetaBlkMgrTest(VMetaBlkMgrTest&&) noexcept = delete;
    VMetaBlkMgrTest& operator=(VMetaBlkMgrTest&) noexcept = delete;

    virtual ~VMetaBlkMgrTest() override = default;

protected:
    void SetUp() override{};

    void TearDown() override{};

public:
    [[nodiscard]] uint64_t get_elapsed_time(const Clock::time_point& start) {
        const std::chrono::seconds sec{std::chrono::duration_cast< std::chrono::seconds >(Clock::now() - start)};
        return sec.count();
    }

    [[nodiscard]] bool keep_running() {
        HS_DBG_ASSERT(m_mbm->get_size() >= m_mbm->get_used_size(), "total size:{} less than used size: {}",
                      m_mbm->get_size(), m_mbm->get_used_size());
        const auto free_size{m_mbm->get_size() - m_mbm->get_used_size()};
        if (free_size < gp.max_wrt_sz) { return false; }
        if ((get_elapsed_time(m_start_time) >= gp.run_time) || (io_cnt() >= gp.num_io)) { return false; }
        return true;
    }

    [[nodiscard]] uint64_t io_cnt() const { return m_update_cnt + m_wrt_cnt + m_rm_cnt; }

    void gen_rand_buf(uint8_t* const s, const uint32_t len) {
        if (gp.is_bitmap) {
            BitsGenerator::gen_random_bits(len, s);
        } else {
            constexpr std::array< char, 62 > alphanum{
                '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
                'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
            std::random_device rd;
            std::default_random_engine re{rd()};
            std::uniform_int_distribution< size_t > alphanum_rand{0, alphanum.size() - 1};
            for (size_t i{0}; i < len - 1; ++i) {
                s[i] = alphanum[alphanum_rand(re)];
            }
            s[len - 1] = 0;
        }
    }

    // size between 512 ~ 8192, 512 aligned;
    [[nodiscard]] uint32_t rand_size(const bool overflow, const bool aligned = true) {
        static thread_local std::random_device rd;
        static thread_local std::default_random_engine re{rd()};
        if (overflow) {
            std::uniform_int_distribution< long unsigned > dist{gp.min_wrt_sz, gp.max_wrt_sz};
            return aligned ? sisl::round_up(dist(re), dma_address_boundary) : dist(re);
        } else {
            std::uniform_int_distribution< long unsigned > dist{64, m_mbm->meta_blk_context_sz()};
            return dist(re);
        }
    }

    [[nodiscard]] uint64_t total_size_written(const void* const cookie) { return m_mbm->get_meta_size(cookie); }

    void do_write_to_full() {
        static constexpr uint64_t blkstore_overhead = 4 * 1024ul * 1024ul; // 4MB
        ssize_t free_size{static_cast< ssize_t >(m_mbm->get_size() - m_mbm->get_used_size() - blkstore_overhead)};

        HS_REL_ASSERT_GT(free_size, 0);
        HS_REL_ASSERT_EQ(static_cast< uint64_t >(free_size),
                         m_mbm->get_available_blks() * m_mbm->get_page_size() - blkstore_overhead);

        uint64_t size_written{0};
        while (free_size > 0) {
            if (free_size >= gp.max_wrt_sz) {
                size_written = do_sb_write(do_overflow());
            } else {
                size_written = do_sb_write(false /* overflow */, m_mbm->meta_blk_context_sz());
                HS_REL_ASSERT_EQ(size_written, m_mbm->get_page_size());
            }

            // size_written should be at least one page;
            HS_REL_ASSERT_GE(size_written, m_mbm->get_page_size());

            free_size -= size_written;

            HS_REL_ASSERT_EQ(static_cast< uint64_t >(free_size),
                             m_mbm->get_available_blks() * m_mbm->get_page_size() - blkstore_overhead);
        }

        HS_REL_ASSERT_EQ(free_size, 0);
    }

    [[nodiscard]] uint64_t do_sb_write(const bool overflow, size_t sz_to_wrt = 0) {
        ++m_wrt_cnt;
        if (!sz_to_wrt) { sz_to_wrt = rand_size(overflow); }
        int64_t ret_size_written{0};
        uint8_t* const buf{iomanager.iobuf_alloc(512, sz_to_wrt)};
        gen_rand_buf(buf, sz_to_wrt);

        void* cookie{nullptr};
        m_mbm->add_sub_sb(mtype, buf, sz_to_wrt, cookie);
        HS_DBG_ASSERT_NE(cookie, nullptr);

        // LOGINFO("buf written: size: {}, data: {}", sz_to_wrt, (char*)buf);
        meta_blk* const mblk{static_cast< meta_blk* >(cookie)};
        // verify context_sz

        if (mblk->hdr.h.compressed == false) {
            if (overflow) {
                HS_DBG_ASSERT_GE(sz_to_wrt, m_mbm->get_page_size());
                HS_DBG_ASSERT(mblk->hdr.h.ovf_bid.is_valid(), "Expected valid ovf meta blkid");
            } else {
                HS_DBG_ASSERT_LE(sz_to_wrt, m_mbm->meta_blk_context_sz());
                HS_DBG_ASSERT(!mblk->hdr.h.ovf_bid.is_valid(), "Expected invalid ovf meta blkid");
            }

            // verify context_sz
            HS_DBG_ASSERT(mblk->hdr.h.context_sz == sz_to_wrt, "context_sz mismatch: {}/{}",
                          static_cast< uint64_t >(mblk->hdr.h.context_sz), sz_to_wrt);
        }

        {
            // save cookie;
            std::unique_lock< std::mutex > lg{m_mtx};
            const auto bid{mblk->hdr.h.bid.to_integer()};
            HS_DBG_ASSERT(m_write_sbs.find(bid) == m_write_sbs.end(), "cookie already in the map.");

            // save to cache
            m_write_sbs[bid].cookie = cookie;
            m_write_sbs[bid].str = std::string(reinterpret_cast< const char* >(buf), sz_to_wrt);

            ret_size_written = total_size_written(cookie);
            m_total_wrt_sz += ret_size_written;
            HS_DBG_ASSERT(m_total_wrt_sz == m_mbm->get_used_size(), "Used size mismatch: {}/{}", m_total_wrt_sz,
                          m_mbm->get_used_size());
        }

        static bool done_read{false};
        if (!done_read) {
            done_read = true;
            m_mbm->read_sub_sb(mtype);
            const auto read_buf_str{m_cb_blks[mblk->hdr.h.bid.to_integer()]};
            const std::string write_buf_str{reinterpret_cast< char* >(buf), sz_to_wrt};
            const auto ret{read_buf_str.compare(write_buf_str)};
            if (mblk->hdr.h.compressed == false) {
                HS_DBG_ASSERT(ret == 0, "Context data mismatch: Saved: {}, read: {}.", write_buf_str, read_buf_str);
            }
        }

        iomanager.iobuf_free(buf);

        return ret_size_written;
    }

    void do_sb_remove() {
        void* cookie{nullptr};
        size_t sz{0};
        std::map< uint64_t, sb_info_t >::iterator it;
        {
            static thread_local std::random_device rd;
            static thread_local std::default_random_engine re{rd()};
            std::unique_lock< std::mutex > lg{m_mtx};
            std::uniform_int_distribution< size_t > advance_random{0, m_write_sbs.size() - 1};
            ++m_rm_cnt;
            sz = m_write_sbs.size();
            it = m_write_sbs.begin();
            std::advance(it, advance_random(re));

            cookie = it->second.cookie;
            m_total_wrt_sz -= total_size_written(cookie);
        }

        const auto ret{m_mbm->remove_sub_sb(cookie)};
        if (ret != no_error) { HS_REL_ASSERT(false, "failed to remove subsystem with status: {}", ret.message()); }

        {
            std::unique_lock< std::mutex > lg{m_mtx};
            m_write_sbs.erase(it);
            HS_REL_ASSERT_EQ(sz, m_write_sbs.size() + 1); // release assert to make compiler happy on sz;

            HS_DBG_ASSERT(m_total_wrt_sz == m_mbm->get_used_size(), "Used size mismatch: {}/{}", m_total_wrt_sz,
                          m_mbm->get_used_size());
        }
    }

    void do_single_sb_read() {
        meta_blk* mblk{nullptr};
        std::string str;
        {
            std::unique_lock< std::mutex > lg{m_mtx};
            const auto it{m_write_sbs.begin()};

            HS_DBG_ASSERT_EQ(it != m_write_sbs.end(), true);
            str = it->second.str;
            mblk = static_cast< meta_blk* >(it->second.cookie);
        }

        // read output will be sent via callback which also holds mutex;
        m_mbm->read_sub_sb(mblk->hdr.h.type);

        {
            std::unique_lock< std::mutex > lg{m_mtx};
            const auto read_buf_str{m_cb_blks[mblk->hdr.h.bid.to_integer()]};
            const std::string write_buf_str{str};
            const auto ret{read_buf_str.compare(write_buf_str)};
            HS_DBG_ASSERT(ret == 0, "Context data mismatch: Saved: {}, read: {}.", write_buf_str, read_buf_str);
        }
    }

    void do_sb_update(const bool aligned_buf_size, uint64_t size_to_update = 0) {
        ++m_update_cnt;
        uint8_t* buf{nullptr};
        auto overflow = do_overflow();
        if (!aligned_buf_size) { overflow = true; } // for unaligned buf size, let's generate overflow buf size;
        auto sz_to_wrt{size_to_update ? size_to_update : rand_size(overflow, aligned_buf_size)};
        void* cookie{nullptr};
        bool unaligned_addr{false};
        uint32_t unaligned_shift{0};
        {
            static thread_local std::random_device rd;
            static thread_local std::default_random_engine re{rd()};
            std::unique_lock< std::mutex > lg{m_mtx};
            std::uniform_int_distribution< size_t > advance_random{0, m_write_sbs.size() - 1};
            auto it{m_write_sbs.begin()};
            std::advance(it, advance_random(re));
            if (aligned_buf_size) {
                buf = iomanager.iobuf_alloc(512, sz_to_wrt);
            } else {
                // do unaligned write
                buf = new uint8_t[sz_to_wrt];
                // simulate some unaligned sz and unaligned buffer address
                if (((reinterpret_cast< std::uintptr_t >(buf) &
                      static_cast< std::uintptr_t >(dma_address_boundary - 1)) == 0x00) &&
                    !do_aligned()) {
                    unaligned_addr = true;
                    std::uniform_int_distribution< long unsigned > dist{1, dma_address_boundary - 1};
                    unaligned_shift = dist(re);
                    HS_DBG_ASSERT_GT(sz_to_wrt, unaligned_shift);
                    buf += unaligned_shift; // simulate unaligned address
                    sz_to_wrt -= unaligned_shift;
                }
            }

            gen_rand_buf(buf, sz_to_wrt);

            cookie = it->second.cookie;
            m_write_sbs.erase(it);

            // update is in-place, the metablk is re-used, ovf-blk is freed then re-allocated
            // so it is okay to decreaase at this point, then add it back after update completes
            m_total_wrt_sz -= total_size_written(cookie);
        }

        m_mbm->update_sub_sb(buf, sz_to_wrt, cookie);

        {
            std::unique_lock< std::mutex > lg{m_mtx};
            const auto bid{static_cast< const meta_blk* >(cookie)->hdr.h.bid.to_integer()};
            HS_DBG_ASSERT(m_write_sbs.find(bid) == m_write_sbs.end(), "cookie already in the map.");
            m_write_sbs[bid].cookie = cookie;
            m_write_sbs[bid].str = std::string{reinterpret_cast< const char* >(buf), sz_to_wrt};

            // verify context_sz
            const meta_blk* const mblk{static_cast< const meta_blk* >(cookie)};
            if (mblk->hdr.h.compressed == false) {
                HS_DBG_ASSERT(mblk->hdr.h.context_sz == sz_to_wrt, "context_sz mismatch: {}/{}",
                              static_cast< uint64_t >(mblk->hdr.h.context_sz), sz_to_wrt);
            }

            // update total size, add size of metablk back
            m_total_wrt_sz += total_size_written(cookie);
            HS_DBG_ASSERT(m_total_wrt_sz == m_mbm->get_used_size(), "Used size mismatch: {}/{}", m_total_wrt_sz,
                          m_mbm->get_used_size());
        }

        if (aligned_buf_size) {
            iomanager.iobuf_free(buf);
        } else {
            if (unaligned_addr) {
                delete[] (buf - unaligned_shift);
            } else {
                delete[] buf;
            }
        }
    }

    // compare m_cb_blks with m_write_sbs;
    void verify_cb_blks() {
        std::unique_lock< std::mutex > lg{m_mtx};
        HS_DBG_ASSERT_EQ(m_cb_blks.size(), m_write_sbs.size());

        for (auto it{std::cbegin(m_write_sbs)}; it != std::cend(m_write_sbs); ++it) {
            const auto bid{it->first};
            auto it_cb{m_cb_blks.find(bid)};

            HS_DBG_ASSERT(it_cb != std::cend(m_cb_blks), "Saved bid during write not found in recover callback.");

            // the saved buf should be equal to the buf received in the recover callback;
            const int ret{it->second.str.compare(it_cb->second)};
            HS_DBG_ASSERT(ret == 0, "Context data mismatch: Saved: {}, callback: {}.", it->second.str, it_cb->second);
        }
    }

    //
    // 1. do a write, make sure compression is triggered.
    // 2. update same meta blk, with data that exceeds compression ratio and thus back off compression.
    //
    void write_compression_backoff() {
        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.metablk.compress_ratio_limit = 100; // this will allow every compression attempt;
            HS_SETTINGS_FACTORY().save();
        });

        LOGINFO("compression ratio limit changed to: {}", m_mbm->get_compress_ratio_limit());

        [[maybe_unused]] const auto write_result = do_sb_write(true /* do_overflow */, 15 * Mi);

        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.metablk.compress_ratio_limit = 0; // this will disallow every compression attempt;
            HS_SETTINGS_FACTORY().save();
        });

        LOGINFO("compression ratio limit changed to: {}", m_mbm->get_compress_ratio_limit());

        // since we only wrote 1 metablk, it will always pick up the same one;
        do_sb_update(true /* aligned */, 12 * Mi);
    }

    void do_rand_load() {
        while (keep_running()) {
            switch (get_op()) {
            case meta_op_type::write: {
                [[maybe_unused]] const auto write_result{do_sb_write(do_overflow())};
            } break;
            case meta_op_type::remove:
                do_sb_remove();
                break;
            case meta_op_type::update:
                do_sb_update(do_aligned());
                break;
            default:
                break;
            }
        }
    }

    [[nodiscard]] bool do_overflow() const {
        static thread_local std::random_device rd;
        static thread_local std::default_random_engine re{rd()};
        if (gp.always_do_overflow) {
            return true;
        } else {
            std::uniform_int_distribution< uint8_t > overflow_rand{0, 1};
            return (overflow_rand(re) == static_cast< uint8_t >(1));
        }
    }

    [[nodiscard]] bool do_aligned() const {
        static thread_local std::random_device rd;
        static thread_local std::default_random_engine re{rd()};
        std::uniform_int_distribution< uint8_t > aligned_rand{0, 1};
        return (aligned_rand(re) == static_cast< uint8_t >(1));
    }

    void recover() {
        // do recover and callbacks will be triggered;
        m_cb_blks.clear();
        m_mbm->recover(false);
    }

    void validate() {
        // verify received blks via callbaks are all good;
        verify_cb_blks();
    }

    void scan_blks() { m_mbm->scan_meta_blks(); }

    [[nodiscard]] meta_op_type get_op() {
        static thread_local bool keep_remove{false};
        // if we hit some high watermark, remove the sbs until hit some low watermark;
        if (100 * m_mbm->get_used_size() / m_mbm->get_size() > 80) {
            keep_remove = true;
            return meta_op_type::remove;
        }

        if (keep_remove) {
            if (100 * m_mbm->get_used_size() / m_mbm->get_size() > 20) {
                return meta_op_type::remove;
            } else {
                // let's start over the test;
                reset_counters();
                // there is some overhead by MetaBlkMgr, such as meta ssb;
                m_total_wrt_sz = m_mbm->get_used_size();
                keep_remove = false;
            }
        }

        if (do_write()) {
            return meta_op_type::write;
        } else if (do_update()) {
            return meta_op_type::update;
        } else {
            return meta_op_type::remove;
        }
    }

    [[nodiscard]] uint64_t total_op_cnt() const { return m_update_cnt + m_wrt_cnt + m_rm_cnt; }

    [[nodiscard]] uint32_t write_ratio() const {
        if (m_wrt_cnt == 0) return 0;
        return (100 * m_wrt_cnt) / total_op_cnt();
    }

    [[nodiscard]] uint32_t update_ratio() const {
        if (m_update_cnt == 0) return 0;
        return (100 * m_update_cnt) / total_op_cnt();
    }

    [[nodiscard]] bool do_update() const {
        if (update_ratio() < gp.per_update) { return true; }
        return false;
    }

    [[nodiscard]] bool do_write() const {
        if (write_ratio() < gp.per_write) { return true; }
        return false;
    }

    void remove_files() {
        /* no need to delete the user created file/disk */
        if (gp.dev_names.size() == 0) {
            auto const ndevices = SISL_OPTIONS["num_devs"].as< uint32_t >();
            for (uint32_t i{0}; i < ndevices; ++i) {
                const std::filesystem::path fpath{META_FILE_PREFIX + std::to_string(i + 1)};
                if (std::filesystem::exists(fpath) && std::filesystem::is_regular_file(fpath)) {
                    std::filesystem::remove(fpath);
                }
            }
        }
    }

    void shutdown() {
        LOGINFO("shutting down homeblks");
        remove_files();
        VolInterface::shutdown();
        {
            std::unique_lock< std::mutex > lk(m_mtx);
            reset_counters();
            m_write_sbs.clear();
            m_cb_blks.clear();
        }
        LOGINFO("stopping iomgr");
        iomanager.stop();
    }

    void reset_counters() {
        m_wrt_cnt = 0;
        m_update_cnt = 0;
        m_rm_cnt = 0;
        m_total_wrt_sz = 0;
    }

    void register_client() {
        m_mbm = MetaBlkMgrSI();
        m_total_wrt_sz = m_mbm->get_used_size();

        HS_REL_ASSERT_EQ(m_mbm->get_size() - m_total_wrt_sz, m_mbm->get_available_blks() * m_mbm->get_page_size());

        m_mbm->deregister_handler(mtype);
        m_mbm->register_handler(
            mtype,
            [this](meta_blk* mblk, sisl::byte_view buf, size_t size) {
                if (mblk) {
                    std::unique_lock< std::mutex > lg{m_mtx};
                    m_cb_blks[mblk->hdr.h.bid.to_integer()] =
                        std::string{reinterpret_cast< const char* >(buf.bytes()), size};
                }
            },
            [this](bool success) { HS_DBG_ASSERT_EQ(success, true); });
    }

#ifdef _PRERELEASE
    void set_flip_point(const std::string flip_name) {
        FlipCondition null_cond;
        FlipFrequency freq;
        freq.set_count(1);
        freq.set_percent(100);
        m_fc.inject_noreturn_flip(flip_name, {null_cond}, freq);
        LOGDEBUG("Flip " + flip_name + " set");
    }
#endif

    uint64_t m_wrt_cnt{0};
    uint64_t m_update_cnt{0};
    uint64_t m_rm_cnt{0};
    uint64_t m_total_wrt_sz{0};
    Clock::time_point m_start_time;
    MetaBlkMgr* m_mbm{nullptr};
    std::map< uint64_t, sb_info_t > m_write_sbs; // during write, save blkid to buf map;
    std::map< uint64_t, std::string > m_cb_blks; // during recover, save blkid to buf map;
    std::mutex m_mtx;
#ifdef _PRERELEASE
    FlipClient m_fc{HomeStoreFlip::instance()};
#endif
};

static constexpr uint64_t MIN_DRIVE_SIZE{2147483648}; // 2 GB

TEST_F(VMetaBlkMgrTest, min_drive_size_test) {
    start_homestore(1, MIN_DRIVE_SIZE, gp.num_threads);
    mtype = "Test_Min_Drive_Size";
    this->register_client();

    EXPECT_GT(this->do_sb_write(false), static_cast< uint64_t >(0));

    this->do_single_sb_read();

    this->shutdown();
}

TEST_F(VMetaBlkMgrTest, write_to_full_test) {
    start_homestore(SISL_OPTIONS["num_devs"].as< uint32_t >(),
                    SISL_OPTIONS["dev_size_gb"].as< uint64_t >() * 1024 * 1024 * 1024, gp.num_threads);
    mtype = "Test_Write_to_Full";
    reset_counters();
    m_start_time = Clock::now();
    this->register_client();

    this->do_write_to_full();

    this->shutdown();
}

TEST_F(VMetaBlkMgrTest, single_read_test) {
    start_homestore(SISL_OPTIONS["num_devs"].as< uint32_t >(),
                    SISL_OPTIONS["dev_size_gb"].as< uint64_t >() * 1024 * 1024 * 1024, gp.num_threads);
    mtype = "Test_Read";
    reset_counters();
    m_start_time = Clock::now();
    this->register_client();

    EXPECT_GT(this->do_sb_write(false), static_cast< uint64_t >(0));

    this->do_single_sb_read();

    this->shutdown();
}

// 1. randome write, update, remove;
// 2. recovery test and verify callback context data matches;
TEST_F(VMetaBlkMgrTest, random_load_test) {
    start_homestore(SISL_OPTIONS["num_devs"].as< uint32_t >(),
                    SISL_OPTIONS["dev_size_gb"].as< uint64_t >() * 1024 * 1024 * 1024, gp.num_threads);
    mtype = "Test_Rand_Load";
    reset_counters();
    m_start_time = Clock::now();
    register_client();

    this->do_rand_load();

    // simulate reboot case that MetaBlkMgr will scan the disk for all the metablks that were written;
    this->scan_blks();

    this->recover();

    this->validate();

    this->shutdown();
}
TEST_F(VMetaBlkMgrTest, get_status_test) {
    start_homestore(SISL_OPTIONS["num_devs"].as< uint32_t >(),
                    SISL_OPTIONS["dev_size_gb"].as< uint64_t >() * 1024 * 1024 * 1024, gp.num_threads);
    auto validate_status = [this](std::string type, uint64_t size, bool expected_error=false) {
        sisl::status_request status_req;
        status_req.obj_name = "MetaBlk_"+type;
        status_req.verbose_level = 3;
        const auto sobject_mgr = HomeStoreBase::safe_instance()->sobject_mgr();
        const auto status_resp = sobject_mgr->get_status(status_req);
        LOGINFO("get_status returned : {}", status_resp.json.dump());
        if(status_resp.json.contains("error")) {
             ASSERT_TRUE(expected_error);
        }
        if(status_resp.json.contains("[0] content")){
            auto encoded_content = status_resp.json["[0] content"];
            const auto decode_content = hs_utils::decodeBase64(encoded_content);
            auto val_content = hs_utils::encodeBase64(reinterpret_cast< const unsigned char* >(decode_content.data()),
                                                      decode_content.size());
            ASSERT_EQ(encoded_content, val_content);
            ASSERT_EQ(!expected_error, decode_content.size() == size);
        }
    };

    mtype = "Test_Write";
    reset_counters();
    m_start_time = Clock::now();
    register_client();
    [[maybe_unused]] auto write_result = do_sb_write(false, 500);
    validate_status(mtype, 500 ); // check the size written is correct as well as encode/decode

    mtype = "Test_Write2";
    reset_counters();
    register_client();
    write_result = do_sb_write(false, 500);
    validate_status(mtype, 500); // register the second type and validate it

    // simulate reboot case that MetaBlkMgr will scan the disk for all the metablks that were written;
    this->scan_blks();
    this->recover();
    this->validate();
    MetaBlkMgrSI()->deregister_handler(mtype); // deregister the first type and make sure it is not existed
    validate_status("Test_Write", 500);
    validate_status("Test_Write2", 500, true); // expected to have error

    mtype = "Test_Write2"; // agian add the first type with different size
    reset_counters();
    register_client();
    write_result = do_sb_write(false, 100);
    validate_status(mtype, 500, true); // Since the size is 100, the former size must be wrong
    validate_status("ERROR_TYPE", 500, true); // An error type also must results in error


    this->shutdown();
}

#ifdef _PRERELEASE // release build doens't have flip point
//
// 1. Turn on flip to simulate fix is not there;
// 2. Write compressed then uncompressed to reproduce the issue which ends up writing bad data (hdr size mismatch) to
// disk, Change dynamic setting to skip hdr size check, because we've introduced bad data.
// 3. Do a recover, verify no crash or assert should happen (need the code change to recover a bad data during
// scan_meta_blks) and data can be fixed during recovery and send back to consumer;
// 4. After recovery everything should be fine;
//
TEST_F(VMetaBlkMgrTest, RecoveryFromBadData) {
    start_homestore(1, MIN_DRIVE_SIZE, gp.num_threads);
    mtype = "Test_Recovery_from_bad_data";
    reset_counters();
    m_start_time = Clock::now();
    this->register_client();

    set_flip_point("without_compress_init");
    //
    // 1. write compressed metablk
    // 2. do an update on the metablk with compression ratio not meet limit, so backoff compression.
    // 3. bad data (with size mismatch) will be written which is expected;
    //
    this->write_compression_backoff();

    HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
        s.metablk.skip_header_size_check =
            1; // this will skip hdr size mismatch check during recovery, because we know bad data is written;
        HS_SETTINGS_FACTORY().save();
    });

    LOGINFO("skip_header_size_check changed to: {}", m_mbm->get_skip_hdr_check());

    //
    // Then do a recovery, the data read from disk should be uncompressed and match the size we saved in its metablk
    // header. If size mismatch, it will hit assert failure;
    //

    // simulate reboot case that MetaBlkMgr will scan the disk for all the metablks that were written;
    this->scan_blks();

    this->recover();

    this->validate();

    // up to this point, we can not use the cached meta blk to keep doing update because the mblk in memory copy inside
    // metablkstore are all freed;

    this->shutdown();
}
#endif

TEST_F(VMetaBlkMgrTest, CompressionBackoff) {
    start_homestore(1, MIN_DRIVE_SIZE, gp.num_threads);
    mtype = "Test_Compression_Backoff";
    reset_counters();
    m_start_time = Clock::now();
    this->register_client();

    //
    // 1. write compressed metablk
    // 2. do an update on the metablk with compression ratio not meet limit, so backoff compression.
    //
    this->write_compression_backoff();

    //
    // Then do a recovery, the data read from disk should be uncompressed and match the size we saved in its metablk
    // header. If size mismatch, it will hit assert failure;
    //

    // simulate reboot case that MetaBlkMgr will scan the disk for all the metablks that were written;
    this->scan_blks();

    this->recover();

    this->validate();

    this->shutdown();
}

SISL_OPTION_GROUP(
    test_meta_blk_mgr,
    (num_threads, "", "num_threads", "number of threads", ::cxxopts::value< uint32_t >()->default_value("2"), "number"),
    (num_devs, "", "num_devs", "number of devices to create", ::cxxopts::value< uint32_t >()->default_value("2"),
     "number"),
    (device_list, "", "device_list", "List of device paths", ::cxxopts::value< std::vector< std::string > >(),
     "path [...]"),
    (fixed_write_size_enabled, "", "fixed_write_size_enabled", "fixed write size enabled 0 or 1",
     ::cxxopts::value< uint32_t >()->default_value("0"), "flag"),
    (fixed_write_size, "", "fixed_write_size", "fixed write size", ::cxxopts::value< uint32_t >()->default_value("512"),
     "number"),
    (dev_size_gb, "", "dev_size_gb", "size of each device in GB", ::cxxopts::value< uint64_t >()->default_value("5"),
     "number"),
    (run_time, "", "run_time", "running time in seconds", ::cxxopts::value< uint64_t >()->default_value("30"),
     "number"),
    (min_write_size, "", "min_write_size", "minimum write size", ::cxxopts::value< uint32_t >()->default_value("4096"),
     "number"),
    (max_write_size, "", "max_write_size", "maximum write size",
     ::cxxopts::value< uint32_t >()->default_value("524288"), "number"),
    (num_io, "", "num_io", "number of io", ::cxxopts::value< uint64_t >()->default_value("300"), "number"),
    (overflow, "", "overflow", "always do overflow", ::cxxopts::value< uint32_t >()->default_value("0"), "number"),
    (per_update, "", "per_update", "update percentage", ::cxxopts::value< uint32_t >()->default_value("20"), "number"),
    (per_write, "", "per_write", "write percentage", ::cxxopts::value< uint32_t >()->default_value("60"), "number"),
    (per_remove, "", "per_remove", "remove percentage", ::cxxopts::value< uint32_t >()->default_value("20"), "number"),
    (bitmap, "", "bitmap", "bitmap test", ::cxxopts::value< bool >()->default_value("false"), "true or false"),
    (spdk, "", "spdk", "spdk", ::cxxopts::value< bool >()->default_value("false"), "true or false"),
    (http_port, "", "http_port", "http_port", ::cxxopts::value< uint32_t >()->default_value("5000"),
     "http server port"));

int main(int argc, char* argv[]) {
    ::testing::GTEST_FLAG(filter) = "*random_load_test*";
    ::testing::InitGoogleTest(&argc, argv);
    SISL_OPTIONS_LOAD(argc, argv, logging, test_meta_blk_mgr);
    sisl::logging::SetLogger("test_meta_blk_mgr");
    spdlog::set_pattern("[%D %T%z] [%^%l%$] [%n] [%t] %v");

    gp.num_io = SISL_OPTIONS["num_io"].as< uint64_t >();
    gp.num_threads = SISL_OPTIONS["num_threads"].as< uint32_t >();
    gp.run_time = SISL_OPTIONS["run_time"].as< uint64_t >();
    gp.per_update = SISL_OPTIONS["per_update"].as< uint32_t >();
    gp.per_write = SISL_OPTIONS["per_write"].as< uint32_t >();
    gp.fixed_wrt_sz_enabled = SISL_OPTIONS["fixed_write_size_enabled"].as< uint32_t >();
    gp.fixed_wrt_sz = SISL_OPTIONS["fixed_write_size"].as< uint32_t >();
    gp.min_wrt_sz = SISL_OPTIONS["min_write_size"].as< uint32_t >();
    gp.max_wrt_sz = SISL_OPTIONS["max_write_size"].as< uint32_t >();
    gp.always_do_overflow = SISL_OPTIONS["overflow"].as< uint32_t >();
    gp.is_spdk = SISL_OPTIONS["spdk"].as< bool >();
    gp.is_bitmap = SISL_OPTIONS["bitmap"].as< bool >();

    if (SISL_OPTIONS.count("device_list")) {
        gp.dev_names = SISL_OPTIONS["device_list"].as< std::vector< std::string > >();
        std::string dev_list_str;
        for (const auto& d : gp.dev_names) {
            dev_list_str += d;
        }
        LOGINFO("Taking input dev_list: {}", dev_list_str);
    }

    if ((gp.per_update == 0) || (gp.per_write == 0) || (gp.per_update + gp.per_write + gp.per_remove != 100)) {
        gp.per_update = 20;
        gp.per_write = 60;
        gp.per_remove = 20;
    }

    if ((gp.max_wrt_sz < gp.min_wrt_sz) || (gp.min_wrt_sz < 4096)) {
        gp.min_wrt_sz = 4096;
        gp.max_wrt_sz = 65536;
        LOGINFO("Invalid input for min/max wrt sz: defaulting to {}/{}", gp.min_wrt_sz, gp.max_wrt_sz);
    }

    /* if --spdk is not set, check env variable if user want to run spdk */
    if (!gp.is_spdk && std::getenv(SPDK_ENV_VAR_STRING.c_str())) { gp.is_spdk = true; }
    if (gp.is_spdk) { gp.num_threads = 2; }

    LOGINFO("Testing with spdk: {}, run_time: {}, num_io: {}, overflow: {}, write/update/remove percentage: {}/{}/{}, "
            "min/max io "
            "size: {}/{}",
            gp.is_spdk, gp.run_time, gp.num_io, gp.always_do_overflow, gp.per_write, gp.per_update, gp.per_remove,
            gp.min_wrt_sz, gp.max_wrt_sz);
    const auto res{RUN_ALL_TESTS()};

    return res;
}
