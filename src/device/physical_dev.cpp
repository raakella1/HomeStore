/*
 * PhysicalDev.cpp
 *
 *  Created on: 05-Aug-2016
 *      Author: hkadayam
 */

#include "device.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <iostream>
#include <folly/Exception.h>
#include <boost/utility.hpp>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h> 
#include <endpoint/drive_endpoint.hpp>
#endif

namespace homestore {
using namespace homeio;

DriveEndPoint *PhysicalDev::ep = NULL;

#ifdef __APPLE__

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    lseek(fd, offset, SEEK_SET);
    return ::readv(fd, iov, iovcnt);
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {

    lseek(fd, offset, SEEK_SET);
    return ::writev(fd, iov, iovcnt);
}

#endif

static std::atomic< uint64_t > glob_phys_dev_offset(0);
static std::atomic< uint32_t > glob_phys_dev_ids(0);

void
PhysicalDev::update(uint32_t dev_num, uint64_t dev_offset, uint32_t first_chunk_id) {
    
    assert(m_info_blk.dev_num == INVALID_DEV_ID);
    assert(m_info_blk.first_chunk_id == INVALID_CHUNK_ID);

    m_info_blk.dev_num = dev_num;
    m_info_blk.dev_offset = dev_offset;
    m_info_blk.first_chunk_id = first_chunk_id;
}

void
PhysicalDev::attach_superblock_chunk(PhysicalDevChunk *chunk) {
    if (!m_superblock_valid) {
        assert(m_dm_chunk[m_cur_indx] == nullptr);
        assert(m_cur_indx < 2);
        m_dm_chunk[m_cur_indx++] = chunk;
    }
    if (chunk->get_chunk_id() == m_super_blk->dm_chunk[0].chunk_id) {
        assert(m_dm_chunk[0] == nullptr);
        m_dm_chunk[0] = chunk;
    } else {
        assert(chunk->get_chunk_id() == m_super_blk->dm_chunk[1].chunk_id);
        assert(m_dm_chunk[1] == nullptr);
        m_dm_chunk[1] = chunk;
    }
}

PhysicalDev::PhysicalDev(DeviceManager *mgr,
                         const std::string& devname,
                         int const oflags,
                         std::shared_ptr<iomgr::ioMgr> iomgr,
                         homeio::comp_callback cb,
                         boost::uuids::uuid uuid,
                         uint32_t dev_num, uint64_t dev_offset, uint32_t is_file, bool is_init, uint64_t dm_info_size, 
                         bool &is_inited) :
        m_mgr(mgr),
        m_devname(devname),
        m_comp_cb(cb),
        m_iomgr(iomgr),
        m_metrics("Physical_Device_" + devname) {

    struct stat stat_buf;
    stat(devname.c_str(), &stat_buf);
    m_devsize = (uint64_t) stat_buf.st_size;
    assert(sizeof(super_block) <= SUPERBLOCK_SIZE);
    auto ret = posix_memalign((void **) &m_super_blk, HomeStoreConfig::align_size, SUPERBLOCK_SIZE);
    /* super block should always be written atomically. */
    assert(m_super_blk != nullptr);
    assert(sizeof(super_block) <= HomeStoreConfig::atomic_phys_page_size);

    if (!ep) {
    	ep = new DriveEndPoint(iomgr, cb); 
    }
    
    m_info_blk.uuid = uuid;
    m_info_blk.dev_num = dev_num;
    m_info_blk.dev_offset = dev_offset;
    m_info_blk.first_chunk_id = INVALID_CHUNK_ID;
    m_cur_indx = 0;
    m_superblock_valid = false;

    m_devfd = ep->open_dev(devname.c_str(), oflags);
    if (m_devfd == -1) {
        throw std::system_error(errno, std::system_category(), "error while opening the device");
    }

    if (is_file) {
        struct stat buf;
        if (fstat(m_devfd, &buf) < 0) {
            assert(0);
            throw std::system_error(errno, std::system_category(), "error while getting size of the device");
        }
        m_devsize = buf.st_size;
    } else {
        if (ioctl(m_devfd, BLKGETSIZE64, &m_devsize) < 0) {
            assert(0);
            throw std::system_error(errno, std::system_category(), "error while getting size of the device");
        }
    }
    assert(m_devsize > 0);
    m_dm_chunk[0] = m_dm_chunk[1] = nullptr;
    if (is_init) {
        /* create a chunk */
        uint64_t align_size = ALIGN_SIZE(SUPERBLOCK_SIZE, HomeStoreConfig::phys_page_size);
        assert((get_size() % HomeStoreConfig::phys_page_size) == 0);
        m_mgr->create_new_chunk(this, SUPERBLOCK_SIZE, get_size() - align_size, nullptr); 
        
        /* create two chunks for super blocks */
        for (int i = 0; i < 2; ++i) {
            uint64_t align_size = ALIGN_SIZE(dm_info_size, HomeStoreConfig::phys_page_size);
            assert(align_size == dm_info_size);
            m_dm_chunk[i] = m_mgr->alloc_chunk(this, INVALID_VDEV_ID, align_size, INVALID_CHUNK_ID);
            m_dm_chunk[i]->set_sb_chunk();
        }
        /* super block is written when first DM info block is written. Writing a superblock and making
         * a disk valid before that doesn't make sense as that disk is of no use until DM info is not
         * written.
         */
    } else {
        is_inited = load_super_block();
        /* If it is different then it mean it require upgrade/revert handling */
        assert(m_super_blk->dm_chunk[0].chunk_size == dm_info_size);
        assert(m_super_blk->dm_chunk[1].chunk_size == dm_info_size);
    }
}

bool PhysicalDev::load_super_block() {
    memset(m_super_blk, 0, SUPERBLOCK_SIZE);
    
    read_superblock();
    
    // Validate if its homestore formatted device
    
    bool is_omstore_dev = validate_device();
    if (!is_omstore_dev) {
        return false;
    }
    if (m_super_blk->this_dev_info.uuid != m_info_blk.uuid) {
        assert(0);
        throw std::system_error(errno, std::system_category(), "uuid mismatch");
    }
    m_info_blk.dev_num = m_super_blk->this_dev_info.dev_num;
    m_info_blk.dev_offset = m_super_blk->this_dev_info.dev_offset;
    m_info_blk.first_chunk_id = m_super_blk->this_dev_info.first_chunk_id;
    m_cur_indx = m_super_blk->cur_indx;
    m_superblock_valid = true;

    return true;
}

void PhysicalDev::read_dm_chunk(char *mem, uint64_t size) {
    assert(m_super_blk->dm_chunk[m_cur_indx % 2].chunk_size == size);
    auto offset =  m_super_blk->dm_chunk[m_cur_indx % 2].chunk_start_offset; 
    ep->sync_read(get_devfd(), mem, size, (off_t) offset);
}

void PhysicalDev::write_dm_chunk(uint64_t gen_cnt, char *mem, uint64_t size) {
    auto offset = m_dm_chunk[(++m_cur_indx) % 2]->get_start_offset();
    ep->sync_write(get_devfd(), mem, size, (off_t) offset);
    write_super_block(gen_cnt);
}

uint64_t PhysicalDev::sb_gen_cnt() {
    return m_super_blk->gen_cnt;
}

void PhysicalDev::write_super_block(uint64_t gen_cnt) {

    // Format the super block and this device info structure
    m_super_blk->magic = MAGIC;
    strcpy(m_super_blk->product_name, PRODUCT_NAME);
    m_super_blk->version = CURRENT_SUPERBLOCK_VERSION;
    
    assert(m_info_blk.dev_num != INVALID_DEV_ID);
    assert(m_info_blk.first_chunk_id != INVALID_CHUNK_ID);
    
    m_super_blk->this_dev_info.uuid = m_info_blk.uuid;
    m_super_blk->this_dev_info.dev_num = m_info_blk.dev_num;
    m_super_blk->this_dev_info.first_chunk_id = m_info_blk.first_chunk_id;
    m_super_blk->this_dev_info.dev_offset = m_info_blk.dev_offset;
    m_super_blk->gen_cnt = gen_cnt;
    m_super_blk->cur_indx = m_cur_indx;

    for (int i = 0; i < 2; i++) {
        memcpy(&m_super_blk->dm_chunk[i], m_dm_chunk[i]->get_chunk_info(), sizeof(chunk_info_block));
    }

    // Write the information to the offset
    write_superblock();
    m_superblock_valid = true;
}

inline bool PhysicalDev::validate_device() {
    return ((m_super_blk->magic == MAGIC) &&
            (strcmp(m_super_blk->product_name, "OmStore") == 0) &&
            (m_super_blk->version == CURRENT_SUPERBLOCK_VERSION));
}

inline void PhysicalDev::write_superblock() {
    ssize_t bytes = pwrite(m_devfd, m_super_blk, SUPERBLOCK_SIZE, 0);
    if (unlikely((bytes < 0) || (size_t)bytes != SUPERBLOCK_SIZE)) {
        throw std::system_error(errno, std::system_category(), "error while writing a superblock" + get_devname());
    }
}

inline void PhysicalDev::read_superblock() {
    memset(m_super_blk, 0, SUPERBLOCK_SIZE);

    ssize_t bytes = pread(m_devfd, m_super_blk, SUPERBLOCK_SIZE, 0);
    if (unlikely((bytes < 0) || ((size_t)bytes != SUPERBLOCK_SIZE))) {
        throw std::system_error(errno, std::system_category(), "error while reading a superblock" + get_devname());
    }
}

void PhysicalDev::write(const char *data, uint32_t size, uint64_t offset, uint8_t *cookie) {
    ep->async_write(get_devfd(), data, size, (off_t) offset, cookie);
}

void PhysicalDev::writev(const struct iovec *iov, int iovcnt, uint32_t size, uint64_t offset, 
								uint8_t *cookie) {
    ep->async_writev(get_devfd(), iov, iovcnt, size, offset, cookie);
}

void PhysicalDev::read(char *data, uint32_t size, uint64_t offset, uint8_t *cookie) {
    ep->async_read(get_devfd(), data, size, (off_t) offset, cookie);
}

void PhysicalDev::readv(const struct iovec *iov, int iovcnt, uint32_t size, uint64_t offset, 
						uint8_t *cookie) {
    ep->async_readv(get_devfd(), iov, iovcnt, size, (off_t) offset, cookie);
}

void PhysicalDev::sync_write(const char *data, uint32_t size, uint64_t offset) {
    try {
        ep->sync_write(get_devfd(), data, size, (off_t) offset);
    } catch (const std::system_error& e) {
        std::stringstream ss;
        ss << "dev_name " << get_devname() << ":" << e.what() << "\n";
        const std::string s = ss.str();
        throw std::system_error(e.code(), s);
    }
}

void PhysicalDev::sync_writev(const struct iovec *iov, int iovcnt, 
					uint32_t size, uint64_t offset) {
    try {
        ep->sync_writev(get_devfd(), iov, iovcnt, size, (off_t) offset);
    } catch (const std::system_error& e) {
        std::stringstream ss;
        ss << "dev_name " << get_devname() << e.what() << "\n";
        const std::string s = ss.str();
        throw std::system_error(e.code(), s);
    }
}

void PhysicalDev::sync_read(char *data, uint32_t size, uint64_t offset) {
    try {
        ep->sync_read(get_devfd(), data, size, (off_t) offset);
    } catch (const std::system_error& e) {
        std::stringstream ss;
        ss << "dev_name " << get_devname() << e.what() << "\n";
        const std::string s = ss.str();
        throw std::system_error(e.code(), s);
    }

}

void PhysicalDev::sync_readv(const struct iovec *iov, int iovcnt, uint32_t size, uint64_t offset) {
    try {
        ep->sync_readv(get_devfd(), iov, iovcnt, size, (off_t) offset);
    } catch (const std::system_error& e) {
        std::stringstream ss;
        ss << "dev_name " << get_devname() << e.what() << "\n";
        const std::string s = ss.str();
        throw std::system_error(e.code(), s);
    }
}

void PhysicalDev::attach_chunk(PhysicalDevChunk *chunk, PhysicalDevChunk *after) {
    if (after) {
        chunk->set_next_chunk(after->get_next_chunk());
        chunk->set_prev_chunk(after);

        auto next = after->get_next_chunk();
        if (next) next->set_prev_chunk(chunk);
        after->set_next_chunk(chunk);
    } else {
        assert(m_info_blk.first_chunk_id == INVALID_CHUNK_ID);
        m_info_blk.first_chunk_id = chunk->get_chunk_id();
    }
}

std::array<uint32_t, 2> PhysicalDev::merge_free_chunks(PhysicalDevChunk *chunk) {
    std::array<uint32_t, 2> freed_ids = {INVALID_CHUNK_ID, INVALID_CHUNK_ID};
    uint32_t nids = 0;

    // Check if previous and next chunk are free, if so make it contiguous chunk
    PhysicalDevChunk *prev_chunk = chunk->get_prev_chunk();
    PhysicalDevChunk *next_chunk = chunk->get_next_chunk();

    if (prev_chunk && !prev_chunk->is_busy()) {
        // We can merge our space to prev_chunk and remove our current chunk.
        prev_chunk->set_size(prev_chunk->get_size() + chunk->get_size());
        prev_chunk->set_next_chunk(chunk->get_next_chunk());

        // Erase the current chunk entry
        prev_chunk->set_next_chunk(chunk->get_next_chunk());
        if (next_chunk) next_chunk->set_prev_chunk(prev_chunk);

        freed_ids[nids++] = chunk->get_chunk_id();
        chunk = prev_chunk;
    }

    if (next_chunk && !next_chunk->is_busy()) {
        next_chunk->set_size(chunk->get_size() + next_chunk->get_size());
        next_chunk->set_start_offset(chunk->get_start_offset());

        // Erase the current chunk entry
        next_chunk->set_prev_chunk(chunk->get_prev_chunk());
        auto p = chunk->get_prev_chunk();
        if (p) p->set_next_chunk(next_chunk);
        freed_ids[nids++] = chunk->get_chunk_id();
    }
    return freed_ids;
}

pdev_info_block
PhysicalDev::get_info_blk() {
    return m_info_blk;
}

PhysicalDevChunk *PhysicalDev::find_free_chunk(uint64_t req_size) {
    // Get the slot with closest size;
    PhysicalDevChunk *closest_chunk = nullptr;

    PhysicalDevChunk *chunk = device_manager()->get_chunk(m_info_blk.first_chunk_id);
    while (chunk) {
        if (!chunk->is_busy() && (chunk->get_size() >= req_size)) {
            if ((closest_chunk == nullptr) || (chunk->get_size() < closest_chunk->get_size())) {
                closest_chunk = chunk;
            }
        }
        chunk = device_manager()->get_chunk(chunk->get_next_chunk_id());
    }

    return closest_chunk;
}

std::string PhysicalDev::to_string() {
    std::stringstream ss;
    ss << "Device name = " << m_devname << "\n";
    ss << "Device fd = " << m_devfd << "\n";
    ss << "Device size = " << m_devsize << "\n";
    ss << "Super Block :\n";
    ss << "\tMagic = " << m_super_blk->magic << "\n";
    ss << "\tProduct Name = " << m_super_blk->product_name << "\n";
    ss << "\tHeader version = " << m_super_blk->version << "\n";
    ss << "\tUUID = " << m_info_blk.uuid << "\n";
    ss << "\tPdev Id = " << m_info_blk.dev_num << "\n";
    ss << "\tPdev Offset = " << m_info_blk.dev_offset << "\n";
    ss << "\tFirst chunk id = " << m_info_blk.first_chunk_id << "\n";

    PhysicalDevChunk *pchunk = device_manager()->get_chunk(m_info_blk.first_chunk_id);
    while (pchunk) {
        ss << "\t\t" << pchunk->to_string() << "\n";
        pchunk = pchunk->get_next_chunk();
    }

    return ss.str();
}

/********************* PhysicalDevChunk Section ************************/
PhysicalDevChunk::PhysicalDevChunk(PhysicalDev *pdev, chunk_info_block *cinfo) {
    m_chunk_info = cinfo;
    m_pdev = pdev;
#if 0
    const std::unique_ptr< PhysicalDev > &p =
            (static_cast<const homeds::sparse_vector< std::unique_ptr< PhysicalDev > > &>(device_manager()->m_pdevs))[cinfo->pdev_id];
    m_pdev = p.get();
#endif
}

PhysicalDevChunk::PhysicalDevChunk(PhysicalDev *pdev, uint32_t chunk_id, uint64_t start_offset, uint64_t size,
                                   chunk_info_block *cinfo) {
    m_chunk_info = cinfo;
    // Fill in with new chunk info
    m_chunk_info->chunk_id = chunk_id;
    m_chunk_info->slot_allocated = true;
    m_chunk_info->pdev_id = pdev->get_dev_id();
    m_chunk_info->chunk_start_offset = start_offset;
    m_chunk_info->chunk_size = size;
    m_chunk_info->prev_chunk_id = INVALID_CHUNK_ID;
    m_chunk_info->next_chunk_id = INVALID_CHUNK_ID;
    m_chunk_info->primary_chunk_id = INVALID_CHUNK_ID;
    m_chunk_info->vdev_id = INVALID_VDEV_ID;
    m_chunk_info->is_sb_chunk = false;
    m_pdev = pdev;
}

PhysicalDevChunk* PhysicalDevChunk::get_next_chunk() const {
    return device_manager()->get_chunk(get_next_chunk_id());
}

PhysicalDevChunk* PhysicalDevChunk::get_prev_chunk() const {
    return device_manager()->get_chunk(get_prev_chunk_id());
}

PhysicalDevChunk* PhysicalDevChunk::get_primary_chunk() const {
    return device_manager()->get_chunk(m_chunk_info->primary_chunk_id);
}

DeviceManager *PhysicalDevChunk::device_manager() const {
    return get_physical_dev()->device_manager();
}
} // namespace homestore
