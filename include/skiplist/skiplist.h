#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <vector>

// ************************ SkipListNode ************************
struct SkipListNode {
  std::string key;   // 节点存储的键
  std::string value; // 节点存储的值
  std::vector<std::shared_ptr<SkipListNode>>
      forward; // 指向不同层级的下一个节点的指针数组

  SkipListNode(const std::string &k, const std::string &v, int level)
      : key(k), value(v), forward(level, nullptr) {}
};

// ************************ SkipListIterator ************************

class SkipListIterator {
public:
  // 构造函数，接收锁
  SkipListIterator(std::shared_ptr<SkipListNode> node, std::shared_mutex &mutex)
      : current(node),
        lock(std::make_shared<std::shared_lock<std::shared_mutex>>(mutex)) {}

  // 空迭代器构造函数
  SkipListIterator() : current(nullptr), lock(nullptr) {}

  std::pair<std::string, std::string> operator*() const;

  SkipListIterator &operator++(); // 前置自增

  SkipListIterator operator++(int); // 后置自增

  bool operator==(const SkipListIterator &other) const;

  bool operator!=(const SkipListIterator &other) const;

  std::string get_key() const;
  std::string get_value() const;

  bool is_valid() const;

private:
  std::shared_ptr<SkipListNode> current;
  std::shared_ptr<std::shared_lock<std::shared_mutex>>
      lock; // 持有读锁, 整个迭代器有效期间都持有读锁
};

// ************************ SkipList ************************

class SkipList {
private:
  std::shared_ptr<SkipListNode>
      head; // 跳表的头节点，不存储实际数据，用于遍历跳表
  int max_level;     // 跳表的最大层级数，限制跳表的高度
  int current_level; // 跳表当前的实际层级数，动态变化
  size_t size_bytes = 0; // 跳表当前占用的内存大小（字节数），用于跟踪内存使用
  std::shared_mutex rw_mutex;

  int random_level(); // 生成新节点的随机层级数

public:
  SkipList(int max_lvl = 16); // 构造函数，初始化跳表

  // 析构函数需要确保没有其他线程访问
  ~SkipList() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex);
    // ... 清理资源
  }

  void put(const std::string &key,
           const std::string &value); // 插入或更新键值对

  std::optional<std::string> get(const std::string &key); // 查找键对应的值

  // !!! 这里的 remove 是跳表本身真实的 remove,  lsm 应该使用 put 空值表示删除
  void remove(const std::string &key); // 删除键值对

  std::vector<std::pair<std::string, std::string>>
  flush(); // 将跳表数据刷出，返回有序键值对列表

  size_t get_size();

  void clear(); // 清空跳表，释放内存

  SkipListIterator begin();

  SkipListIterator end();
};