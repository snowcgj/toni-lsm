#pragma once

#include "../../include/memtable/memtable.h"
#include "../../include/sst/sst.h"
#include <cstddef>
#include <memory>
#include <unordered_map>

class LSMEngine {
public:
  std::string data_dir;
  MemTable memtable;
  std::list<size_t> l0_sst_ids;
  std::unordered_map<size_t, std::shared_ptr<SST>> ssts;

  LSMEngine(std::string path);
  ~LSMEngine() = default;

  std::optional<std::string> get(const std::string &key);
  void put(const std::string &key, const std::string &value);
  void remove(const std::string &key);
  void flush();

  std::string get_sst_path(size_t sst_id);
};

class LSM {
private:
  LSMEngine engine;

public:
  LSM(std::string path);
  ~LSM();
};