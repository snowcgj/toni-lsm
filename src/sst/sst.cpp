#include "../../include/sst/sst.h"
#include "../../include/consts.h"
#include "../../include/sst/sst_iterator.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>

// **************************************************
// SST
// **************************************************

std::shared_ptr<SST> SST::open(size_t sst_id, FileObj file,
                               std::shared_ptr<BlockCache> block_cache) {
  auto sst = std::make_shared<SST>();
  sst->sst_id = sst_id;
  sst->file = std::move(file);
  sst->block_cache = block_cache;

  // 读取文件末尾的元数据块
  // 1. 读取元数据块的偏移量, 最后8字节: 2个 uint32_t,
  // 分别是 meta 和 bloom 的 offset
  size_t file_size = sst->file.size();
  if (file_size < sizeof(size_t)) {
    throw std::runtime_error("Invalid SST file: too small");
  }

  auto bloom_offset_bytes =
      sst->file.read_to_slice(file_size - sizeof(uint32_t), sizeof(uint32_t));
  memcpy(&sst->bloom_offset, bloom_offset_bytes.data(), sizeof(uint32_t));

  auto meta_offset_bytes = sst->file.read_to_slice(
      file_size - sizeof(uint32_t) * 2, sizeof(uint32_t));
  memcpy(&sst->meta_block_offset, meta_offset_bytes.data(), sizeof(uint32_t));

  // 2. 读取 bloom filter
  if (sst->bloom_offset + 2 * sizeof(uint32_t) < file_size) {
    // 布隆过滤器偏移量 + 2*uint32_t 的大小小于文件大小
    // 表示存在布隆过滤器
    uint32_t bloom_size = file_size - sst->bloom_offset - sizeof(uint32_t) * 2;
    auto bloom_bytes = sst->file.read_to_slice(sst->bloom_offset, bloom_size);

    auto bloom = BloomFilter::decode(bloom_bytes);
    sst->bloom_filter = std::make_shared<BloomFilter>(std::move(bloom));
  }

  // 3. 读取并解码元数据块
  uint32_t meta_size = sst->bloom_offset - sst->meta_block_offset;
  auto meta_bytes = sst->file.read_to_slice(sst->meta_block_offset, meta_size);
  sst->meta_entries = BlockMeta::decode_meta_from_slice(meta_bytes);

  // 4. 设置首尾key
  if (!sst->meta_entries.empty()) {
    sst->first_key = sst->meta_entries.front().first_key;
    sst->last_key = sst->meta_entries.back().last_key;
  }

  return sst;
}

std::shared_ptr<SST> SST::create_sst_with_meta_only(
    size_t sst_id, size_t file_size, const std::string &first_key,
    const std::string &last_key, std::shared_ptr<BlockCache> block_cache) {
  auto sst = std::make_shared<SST>();
  sst->file.set_size(file_size);
  sst->sst_id = sst_id;
  sst->first_key = first_key;
  sst->last_key = last_key;
  sst->meta_block_offset = 0;
  sst->block_cache = block_cache;

  return sst;
}

std::shared_ptr<Block> SST::read_block(size_t block_idx) {
  if (block_idx >= meta_entries.size()) {
    throw std::out_of_range("Block index out of range");
  }

  // 先从缓存中查找
  if (block_cache != nullptr) {
    auto cache_ptr = block_cache->get(this->sst_id, block_idx);
    if (cache_ptr != nullptr) {
      return cache_ptr;
    }
  } else {
    throw std::runtime_error("Block cache not set");
  }

  const auto &meta = meta_entries[block_idx];
  size_t block_size;

  // 计算block大小
  if (block_idx == meta_entries.size() - 1) {
    block_size = meta_block_offset - meta.offset;
  } else {
    block_size = meta_entries[block_idx + 1].offset - meta.offset;
  }

  // 读取block数据
  auto block_data = file.read_to_slice(meta.offset, block_size);
  auto block_res = Block::decode(block_data, true);

  // 更新缓存
  if (block_cache != nullptr) {
    block_cache->put(this->sst_id, block_idx, block_res);
  } else {
    throw std::runtime_error("Block cache not set");
  }
  return block_res;
}

size_t SST::find_block_idx(const std::string &key) {
  // 先在布隆过滤器判断key是否存在
  if (bloom_filter != nullptr && !bloom_filter->possibly_contains(key)) {
    return -1;
  }

  // 二分查找
  size_t left = 0;
  size_t right = meta_entries.size();

  while (left < right) {
    size_t mid = (left + right) / 2;
    const auto &meta = meta_entries[mid];

    if (key < meta.first_key) {
      right = mid;
    } else if (key > meta.last_key) {
      left = mid + 1;
    } else {
      return mid;
    }
  }

  if (left >= meta_entries.size()) {
    // 如果没有找到完全匹配的块，返回-1
    return -1;
  }
  return left;
}

SstIterator SST::get(const std::string &key) {
  // if (key < first_key || key > last_key) {
  //   return this->end();
  // }

  // 在布隆过滤器判断key是否存在
  if (bloom_filter != nullptr && !bloom_filter->possibly_contains(key)) {
    return this->end();
  }

  return SstIterator(shared_from_this(), key);
}

size_t SST::num_blocks() const { return meta_entries.size(); }

std::string SST::get_first_key() const { return first_key; }

std::string SST::get_last_key() const { return last_key; }

size_t SST::sst_size() const { return file.size(); }

size_t SST::get_sst_id() const { return sst_id; }

SstIterator SST::begin() { return SstIterator(shared_from_this()); }

SstIterator SST::end() {
  SstIterator res(shared_from_this());
  res.m_block_idx = meta_entries.size();
  res.m_block_it = nullptr;
  return res;
}

// **************************************************
// SSTBuilder
// **************************************************

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom) : block(block_size) {
  // 初始化第一个block
  if (has_bloom) {
    bloom_filter = std::make_shared<BloomFilter>(
        BLOOM_FILTER_EXPECTED_SIZE, BLOOM_FILTER_EXPECTED_ERROR_RATE);
  }
  meta_entries.clear();
  data.clear();
  first_key.clear();
  last_key.clear();
}

void SSTBuilder::add(const std::string &key, const std::string &value) {
  // 记录第一个key
  if (first_key.empty()) {
    first_key = key;
  }

  // 在 布隆过滤器 中添加key
  if (bloom_filter != nullptr) {
    bloom_filter->add(key);
  }

  if (block.add_entry(key, value)) {
    // block 满足容量限制, 插入成功
    last_key = key;
    return;
  }

  finish_block(); // 将当前 block 写入

  block.add_entry(key, value);
  first_key = key;
  last_key = key; // 更新最后一个key
}

size_t SSTBuilder::estimated_size() const { return data.size(); }

void SSTBuilder::finish_block() {
  auto old_block = std::move(this->block);
  auto encoded_block = old_block.encode();

  meta_entries.emplace_back(data.size(), first_key, last_key);

  // 计算block的哈希值
  auto block_hash = static_cast<uint32_t>(std::hash<std::string_view>{}(
      std::string_view(reinterpret_cast<const char *>(encoded_block.data()),
                       encoded_block.size())));

  // 预分配空间并添加数据
  data.reserve(data.size() + encoded_block.size() +
               sizeof(uint32_t)); // 加上的是哈希值
  data.insert(data.end(), encoded_block.begin(), encoded_block.end());
  data.resize(data.size() + sizeof(uint32_t));
  memcpy(data.data() + data.size() - sizeof(uint32_t), &block_hash,
         sizeof(uint32_t));
}

std::shared_ptr<SST>
SSTBuilder::build(size_t sst_id, const std::string &path,
                  std::shared_ptr<BlockCache> block_cache) {
  // 完成最后一个block
  if (!block.is_empty()) {
    finish_block();
  }

  // 如果没有数据，抛出异常
  if (meta_entries.empty()) {
    throw std::runtime_error("Cannot build empty SST");
  }

  // 编码元数据块
  std::vector<uint8_t> meta_block;
  BlockMeta::encode_meta_to_slice(meta_entries, meta_block);

  // 计算元数据块的偏移量
  uint32_t meta_offset = data.size();

  // 构建完整的文件内容
  // 1. 已有的数据块
  std::vector<uint8_t> file_content = std::move(data);

  // 2. 添加元数据块
  file_content.insert(file_content.end(), meta_block.begin(), meta_block.end());

  // 3. 编码布隆过滤器
  uint32_t bloom_offset = file_content.size();
  if (bloom_filter != nullptr) {
    auto bf_data = bloom_filter->encode();
    file_content.insert(file_content.end(), bf_data.begin(), bf_data.end());
  }

  file_content.resize(file_content.size() + sizeof(uint32_t) * 2);
  // sizeof(uint32_t) * 2 表示需要分别记录布隆过滤器和元数据块的偏移量

  // 4. 添加元数据块偏移量
  memcpy(file_content.data() + file_content.size() - sizeof(uint32_t) * 2,
         &meta_offset, sizeof(uint32_t));

  // 5. 添加布隆过滤器偏移量
  memcpy(file_content.data() + file_content.size() - sizeof(uint32_t),
         &bloom_offset, sizeof(uint32_t));

  // 创建文件
  FileObj file = FileObj::create_and_write(path, file_content);

  // 返回SST对象
  auto res = std::make_shared<SST>();

  res->sst_id = sst_id;
  res->file = std::move(file);
  res->first_key = meta_entries.front().first_key;
  res->last_key = meta_entries.back().last_key;
  res->meta_block_offset = meta_offset;
  res->bloom_offset = bloom_offset;
  res->meta_entries = std::move(meta_entries);
  res->block_cache = block_cache;

  return res;
}
