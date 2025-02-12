/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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

#include <map>
#include <vector>

#include <iomgr/iomgr.hpp>
#include <sisl/fds/sparse_vector.hpp>
#include <homestore/homestore_decl.hpp>

// Super blk format
//  ________________________________________________________________________________________________________
//  |        |<---------Vdev Area---------->|  <---------------------Chunk Area--------------->|           |
//  | First  | Vdev[1]| Vdev[2]| .. |Vdev[N]| Chunk Slot | Chunk[1] | Chunk[2]| .. |  Chunk[M] | Reserved  |
//  | Block  | Info   | Info   |    | Info  | Bitmap     | Info     | Info    |    |  Info     | Space     |
//  |________|________|________|___ |_______|____________|__________|_________|____|___________|___________|
//
//  where:
//    N = max number of vdevs we support for this class of device
//    M = max number of chunks we support for this class of device

namespace homestore {

#pragma pack(1)
struct disk_attr {
    // all fields in this structure are a copy from iomgr::drive_attributes;
    uint32_t phys_page_size{0};        // Physical page size of flash ssd/nvme. This is optimal size to do IO
    uint32_t align_size{0};            // size alignment supported by drives/kernel
    uint32_t atomic_phys_page_size{0}; // atomic page size of the drive_sync_write_count
    uint32_t num_streams{0};

    disk_attr() = default;
    disk_attr(const iomgr::drive_attributes& iomgr_attr) :
            phys_page_size{iomgr_attr.phys_page_size},
            align_size{iomgr_attr.align_size},
            atomic_phys_page_size{iomgr_attr.atomic_phys_page_size},
            num_streams{iomgr_attr.num_streams} {}

    disk_attr& operator=(const iomgr::drive_attributes& iomgr_attr) {
        phys_page_size = iomgr_attr.phys_page_size;
        align_size = iomgr_attr.align_size;
        atomic_phys_page_size = iomgr_attr.atomic_phys_page_size;
        num_streams = iomgr_attr.num_streams;

        return *this;
    }

    bool is_valid() const {
        return is_page_valid(phys_page_size) && is_page_valid(align_size) && is_page_valid(atomic_phys_page_size);
    }

    bool is_page_valid(uint32_t page_size) const {
        return (page_size == 0 || (page_size & (page_size - 1)) != 0) ? false : true;
    }

    std::string to_string() const {
        return fmt::format("phys_page_size={}, align_size={}, atomic_phys_page_size={}, num_streams={}",
                           in_bytes(phys_page_size), in_bytes(align_size), in_bytes(atomic_phys_page_size),
                           num_streams);
    }
};

struct first_block_header {
    static constexpr const char* PRODUCT_NAME{"OmStore"};
    static constexpr size_t s_product_name_size{64};
    static constexpr uint32_t CURRENT_SUPERBLOCK_VERSION{4};

public:
    uint64_t gen_number{0};                   // Generation count of this structure
    uint32_t version{0};                      // Version Id of this structure
    char product_name[s_product_name_size]{}; // Product name

    uint32_t num_pdevs{0};         // Total number of pdevs homestore is being created on
    uint32_t max_vdevs{0};         // Max VDevs possible, this cannot be changed post formatting
    uint32_t max_system_chunks{0}; // Max Chunks possible, this cannot be changed post formatting
    uuid_t system_uuid;

public:
    const char* get_product_name() const { return product_name; }
    uint32_t get_version() const { return version; }
    uuid_t get_system_uuid() const { return system_uuid; }
    std::string get_system_uuid_str() const { return boost::uuids::to_string(system_uuid); };

    std::string to_string() const {
        auto str = fmt::format("gen_number={}, version={}, product_name={} system_uuid={}", gen_number, get_version(),
                               get_product_name(), get_system_uuid_str());
        return str;
    }
};

struct pdev_info_header {
public:
    uint64_t data_offset{0};         // Offset within pdev where data starts
    uint64_t size{0};                // Total pdev size
    uint32_t pdev_id{0};             // Device ID for this store instance.
    uint32_t max_pdev_chunks{0};     // Max chunks in this pdev possible
    disk_attr dev_attr;              // Attributes homestore expects from all the devices.
    uint8_t mirror_super_block{0x0}; // Have we mirrored the super block on head/tail
    uuid_t system_uuid;              // Current system uuid stamp to protect from device exchange

public:
    std::string to_string() const {
        auto str =
            fmt::format("data_offset={}, size={}, pdev_id={} max_pdev_chunks={} dev_attr=[{}] mirror_super_block?={}",
                        in_bytes(data_offset), in_bytes(size), pdev_id, max_pdev_chunks, dev_attr.to_string(),
                        (mirror_super_block == 0x00) ? "false" : "true");
        return str;
    }

    std::string get_system_uuid_str() const { return boost::uuids::to_string(system_uuid); };
};

struct first_block {
    static constexpr uint32_t s_atomic_fb_size{512};       // increase 512 to actual size if in the future first_block
                                                           // can be larger;
    static constexpr uint32_t s_io_fb_size{4096};          // This is the size we do IO on, with padding
    static constexpr uint32_t HOMESTORE_MAGIC{0xCEEDDEEB}; // Magic written as first bytes on each device

public:
    uint64_t magic{0};              // Header magic expected to be at the top of block
    uint32_t checksum{0};           // Checksum of the entire first block (excluding this field)
    first_block_header hdr;         // Information about the entire system
    pdev_info_header this_pdev_hdr; // Information about the current pdev

public:
    uint64_t get_magic() const { return magic; }

    bool is_valid() const {
        return ((magic == HOMESTORE_MAGIC) &&
                (std::string(hdr.product_name) == std::string(first_block_header::PRODUCT_NAME)));
    }

    std::string to_string() const {
        auto str = fmt::format("magic={:#x}, checksum={}, first_blk_header=[{}], this_pdev_info=[{}]", get_magic(),
                               checksum, hdr.to_string(), this_pdev_hdr.to_string());
        return str;
    }
};
#pragma pack()
static_assert(sizeof(first_block) <= first_block::s_atomic_fb_size);

/////////////// Overarching super block information ////////////////
class hs_super_blk {
public:
    // Minium chunk size we can create in data device. Keeping this lower will increase number of chunks and thus
    // area for super block will be higher.
    static constexpr uint64_t MIN_CHUNK_SIZE_DATA_DEVICE = 16 * 1024 * 1024;

    // Higher min chunk size than data device to ensure to limit max chunks in fast pdevs and thus lesser super block
    // area on more expensive fast device.
    static constexpr uint64_t MIN_CHUNK_SIZE_FAST_DEVICE = 32 * 1024 * 1024;

    // Maximum number of chunks across all devices. We need to keep in mind the BlkId restriction (to address the
    // chunks)
    static constexpr uint32_t MAX_CHUNKS_IN_SYSTEM = 65536;

    // Maximum vdevs in the system. Increasing this will have more vdev information in super block
    static constexpr uint32_t MAX_VDEVS_IN_SYSTEM = 1024;

    static constexpr uint64_t EXTRA_SB_SIZE_FOR_DATA_DEVICE = 8 * 1024 * 1024;
    static constexpr uint64_t EXTRA_SB_SIZE_FOR_FAST_DEVICE = 1 * 1024 * 1024;

    static constexpr uint32_t first_block_offset() { return 0; } // Offset in physical device we can use for first block
    static constexpr uint32_t first_block_size() { return first_block::s_io_fb_size; }
    static uint64_t vdev_super_block_size();
    static uint64_t chunk_super_block_size(const dev_info& dinfo);
    static uint64_t chunk_info_bitmap_size(const dev_info& dinfo) {
        // Chunk bitmap area has bitmap of max_chunks rounded off to 4k page
        return sisl::round_up(std::max(1u, hs_super_blk::max_chunks_in_pdev(dinfo) / 8), 4096);
    }

    static uint64_t total_size(const dev_info& dinfo) { return total_used_size(dinfo) + future_padding_size(dinfo); }
    static uint64_t total_used_size(const dev_info& dinfo) {
        return first_block_size() + vdev_super_block_size() + chunk_super_block_size(dinfo);
    }
    static uint64_t vdev_sb_offset() { return first_block_offset() + first_block_size(); }
    static uint64_t chunk_sb_offset() { return vdev_sb_offset() + vdev_super_block_size(); }

    static uint64_t future_padding_size(const dev_info& dinfo) {
        return (dinfo.dev_type == HSDevType::Fast) ? EXTRA_SB_SIZE_FOR_FAST_DEVICE : EXTRA_SB_SIZE_FOR_DATA_DEVICE;
    }
    static uint32_t max_chunks_in_pdev(const dev_info& dinfo) {
        return (dinfo.dev_size - 1) /
            ((dinfo.dev_type == HSDevType::Fast) ? MIN_CHUNK_SIZE_FAST_DEVICE : MIN_CHUNK_SIZE_DATA_DEVICE) +
            1;
    }
};

} // namespace homestore
