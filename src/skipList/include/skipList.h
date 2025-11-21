#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <random>

// Boost 序列化所需头文件
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp> // 二进制归档
#include <boost/archive/binary_iarchive.hpp> // 二进制归档
#include <boost/serialization/vector.hpp>    // 序列化 vector 必需
#include <boost/serialization/string.hpp>    // 序列化 string 必需
#include <boost/serialization/access.hpp>    // access 必需

#define STORE_FILE "store/dumpFile"

static std::string delimiter = ":";

// Class template to implement node
template <typename K, typename V>
class Node {
 public:
  Node() {}

  Node(K k, V v, int);

  ~Node();

  K get_key() const;

  V get_value() const;

  void set_value(V);

  // Linear array to hold pointers to next node of different level
  Node<K, V> **forward;

  int node_level;

 private:
  K key;
  V value;
};

template <typename K, typename V>
Node<K, V>::Node(const K k, const V v, int level) {
  this->key = k;
  this->value = v;
  this->node_level = level;

  // level + 1, because array index is from 0 - level
  this->forward = new Node<K, V> *[level + 1];

  // Fill forward array with 0(NULL)
  memset(this->forward, 0, sizeof(Node<K, V> *) * (level + 1));
};

template <typename K, typename V>
Node<K, V>::~Node() {
  delete[] forward;
};

template <typename K, typename V>
K Node<K, V>::get_key() const {
  return key;
};

template <typename K, typename V>
V Node<K, V>::get_value() const {
  return value;
};

template <typename K, typename V>
void Node<K, V>::set_value(V value) {
  this->value = value;
};

// Class template to implement node
template <typename K, typename V>
class SkipListDump {
 public:
  friend class boost::serialization::access;

  template <class Archive>
  void serialize(Archive &ar, const unsigned int version) {
    ar &keyDumpVt_;
    ar &valDumpVt_;
  }
  std::vector<K> keyDumpVt_;
  std::vector<V> valDumpVt_;

 public:
  void insert(const Node<K, V> &node);
};

// Class template for Skip list
template <typename K, typename V>
class SkipList {
 public:
  SkipList(int);
  ~SkipList();
  int get_random_level();
  Node<K, V> *create_node(const K& key, const V& value, int level);
  int insert_element(const K& key, const V& value);
  void display_list();
  bool search_element(const K& key, V& value);
  void delete_element(const K& key);
  void insert_set_element(const K& key, const V& value);
  std::string dump_file();
  void load_file(const std::string &dumpStr);
  //递归删除节点
  void clear(Node<K, V> *);
  int size();

 private:
  void get_key_value_from_string(const std::string &str, std::string *key, std::string *value);
  bool is_valid_string(const std::string &str);
  int insert_element_unlocked(const K key, const V value);

 private:
  // Maximum level of the skip list
  int _max_level;

  // current level of skip list
  int _skip_list_level;

  // pointer to header node
  Node<K, V> *_header;

  // file operator
  // std::ofstream _file_writer;
  // std::ifstream _file_reader;

  // skiplist current element count
  int _element_count;

  // std::mutex _mtx;  // mutex for critical section
  std::shared_mutex _mtx;;  // mutex for critical section
};

// create new node
template <typename K, typename V>
Node<K, V> *SkipList<K, V>::create_node(const K& k, const V& v, int level) {
  Node<K, V> *n = new Node<K, V>(k, v, level);
  return n;
}

// Insert given key and value in skip list
// return 1 means element exists
// return 0 means insert successfully
/*
                           +------------+
                           |  insert 50 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100
                                               |    |
                                               |    |
level 2         1          10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 1         1    4     10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 0         1    4   9 10         30   40  | 50 |  60      70       100
                                               +----+

*/

template <typename K, typename V>
int SkipList<K, V>::insert_element_unlocked(const K key, const V value) {
  // 无锁版本
  Node<K, V> *current = this->_header;

  // 2. update 数组
  Node<K, V> *update[_max_level + 1];
  memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

  // 3. 从顶层向下查找插入位置
  for (int i = _skip_list_level; i >= 0; i--) {
      while (current->forward[i] != NULL && current->forward[i]->get_key() < key) {
          current = current->forward[i];
      }
      update[i] = current; // 记录每层的前驱节点
  }

  current = current->forward[0];

  // 4. 检查键是否已存在
  if (current != NULL && current->get_key() == key) {
      // std::cout << "key: " << key << ", exists" << std::endl;
      
      // (改进) 无需手动 unlock()，lock 会在 return 时自动释放
      // _mtx.unlock(); // <--- (旧代码)
      
      return 1; // 存在
  }

  // 5. 键不存在，执行插入
  if (current == NULL || current->get_key() != key) {
      // 6. 获取随机层高
      int random_level = get_random_level();

      // 7. 如果随机层高超过当前最大层高
      if (random_level > _skip_list_level) {
          for (int i = _skip_list_level + 1; i < random_level + 1; i++) {
              update[i] = _header; // 新层的前驱节点是 header
          }
          _skip_list_level = random_level;
      }

      // 8. "缝合" 链表
      Node<K, V> *inserted_node = create_node(key, value, random_level);

      for (int i = 0; i <= random_level; i++) {
          inserted_node->forward[i] = update[i]->forward[i];
          update[i]->forward[i] = inserted_node;
      }
      
      // std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
      _element_count++;
  }

  return 0;
  }

template <typename K, typename V>
int SkipList<K, V>::insert_element(const K& key, const V& value) {
  // 使用 std::unique_lock (获取独占锁)
  std::unique_lock<std::shared_mutex> lock(_mtx);

  // 调用无锁的内部版本
    return insert_element_unlocked(key, value);
}

// Display skip list
template <typename K, typename V>
void SkipList<K, V>::display_list() {
  std::cout << "\n*****Skip List*****"
            << "\n";
  for (int i = 0; i <= _skip_list_level; i++) {
    Node<K, V> *node = this->_header->forward[i];
    std::cout << "Level " << i << ": ";
    while (node != NULL) {
      std::cout << node->get_key() << ":" << node->get_value() << ";";
      node = node->forward[i];
    }
    std::cout << std::endl;
  }
}

// todo 对dump 和 load 后面可能要考虑加锁的问题
// Dump data in memory to file
template <typename K, typename V>
std::string SkipList<K, V>::dump_file() {
   std::cout << "dump_file-----------------" << std::endl;
    //
    // 1. (改进 - 关键) 增加共享锁 (读锁)
    //    这保证了在创建快照期间，跳表结构不会被其他线程修改
    //    保证了线程安全和快照的一致性
    //
    std::shared_lock<std::shared_mutex> lock(_mtx);

    Node<K, V> *node = this->_header->forward[0];
    SkipListDump<K, V> dumper;

    // 遍历第0层链表，将所有 K/V 复制到 dumper DTO 中
    // 这个遍历操作现在是线程安全的
    while (node != nullptr) {
        dumper.insert(*node);
        node = node->forward[0];
    }

    std::stringstream ss;

    //
    // 2. (改进 - 性能) 使用二进制归档
    //    它比文本归档更小、更快
    boost::archive::binary_oarchive oa(ss);
    
    // 序列化 dumper DTO
    oa << dumper;

    return ss.str();

    // 注意：std::shared_lock 会在函数返回时自动释放
}

// Load data from disk
template <typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr) {
    //
    // 1. (改进 - 关键) 获取独占锁 (写锁)
    //    在加载快照的整个过程中持有锁，防止任何并发读/写
    //
    std::unique_lock<std::shared_mutex> lock(_mtx);

    //
    // 2. (改进 - 关键 Bug 修复) 清空当前所有状态
    //    这是 "替换" 语义的实现
    //
    if (_header->forward[0] != nullptr) {
        clear(_header->forward[0]); // 递归删除所有节点
    }
    // 重置 header 的所有指针
    memset(this->_header->forward, 0, sizeof(Node<K, V> *) * (_max_level + 1));
    _skip_list_level = 0;
    _element_count = 0;

    // 3. (改进) 如果快照为空，直接返回
    if (dumpStr.empty()) {
        return;
    }

    // 4. (改进 - 性能/兼容) 使用二进制归档
    SkipListDump<K, V> dumper;
    std::stringstream iss(dumpStr);
    boost::archive::binary_iarchive ia(iss); // <-- 必须与 dump_file 保持一致

    // 5. (改进) 反序列化
    ia >> dumper;

    //
    // 6. (改进 - 关键 Bug 修复 / 性能)
    //
    for (int i = 0; i < dumper.keyDumpVt_.size(); ++i) {
        // (Bug 修复) 使用 valDumpVt_ 而不是 keyDumpVt_
        // (性能) 调用无锁的内部函数，避免死锁和重复加锁
        insert_element_unlocked(dumper.keyDumpVt_[i], dumper.valDumpVt_[i]); 
    }
    
    // 独占锁 lock 会在这里自动释放
}

// Get current SkipList size
template <typename K, typename V>
int SkipList<K, V>::size() {
  return _element_count;
}

template <typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value) {
  if (!is_valid_string(str)) {
    return;
  }
  *key = str.substr(0, str.find(delimiter));
  *value = str.substr(str.find(delimiter) + 1, str.length());
}

template <typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str) {
  if (str.empty()) {
    return false;
  }
  if (str.find(delimiter) == std::string::npos) {
    return false;
  }
  return true;
}

// Delete element from skip list
template <typename K, typename V>
void SkipList<K, V>::delete_element(const K& key) {
    // 1. (改进 - 关键) 使用 std::unique_lock 管理写锁
    //    构造时自动加锁，析构时自动解锁。
    //    配合 shared_mutex，这会阻塞所有的 search 操作，保证数据安全。
    std::unique_lock<std::shared_mutex> lock(_mtx);

    Node<K, V> *current = this->_header;
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // 2. 查找要删除节点的前驱节点
    for (int i = _skip_list_level; i >= 0; i--) {
        while (current->forward[i] != nullptr && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    current = current->forward[0];

    // 3. 检查是否找到目标节点
    if (current != nullptr && current->get_key() == key) {
        
        // 4. 从低层向高层，逐层解链
        for (int i = 0; i <= _skip_list_level; i++) {
            // 如果在第 i 层，前驱节点的下一个节点不是目标节点，
            // 说明目标节点没有达到这一层高度，直接跳出循环
            if (update[i]->forward[i] != current) break;

            // 核心删除操作：前驱指向后继
            update[i]->forward[i] = current->forward[i];
        }

        // 5. 更新跳表的最大层级
        // 如果删除了最高层的节点，且最高层变空了，需要降低层高
        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == nullptr) {
            _skip_list_level--;
        }

        // 6. (改进 - 性能) 移除 std::cout
        // std::cout << "Successfully deleted key " << key << std::endl;
        
        delete current; // 释放内存
        _element_count--;
    }
    
    // 7. (改进) 无需手动 unlock，lock 对象析构时会自动释放锁
}

/**
 * \brief 插入元素。如果键已存在，则更新其值 (Upsert 语义)
 * 改进点：
 * 1. 原子性：全程持有写锁，杜绝竞态条件。
 * 2. 高性能：只进行一次 O(log n) 遍历。
 * 3. 内存优化：如果 key 存在，直接更新 value，避免了 delete+new 的开销。
 */
template <typename K, typename V>
void SkipList<K, V>::insert_set_element(const K& key, const V& value) {
  // 1. 获取独占锁 (写锁)
  std::unique_lock<std::shared_mutex> lock(_mtx);

  Node<K, V> *current = this->_header;
  Node<K, V> *update[_max_level + 1];
  memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

  // 2. 查找位置 (一次遍历)
  for(int i = _skip_list_level; i >= 0; i--) {
    while (current->forward[i] != nullptr && current->forward[i]->get_key() < key) {
        current = current->forward[i];
    }
    update[i] = current;
  }

  current = current->forward[0];

  // 3. 情况 A: 键已存在 -> 原位更新 (Update)
  if (current != nullptr && current->get_key() == key) {
      current->set_value(value);
      // std::cout << "Key already exists, updated value for key: " << key << std::endl;
      return; // 更新完成，直接返回
  }

  // 4. 情况 B: 键不存在 -> 插入新节点 (Insert)
  // (以下逻辑与 insert_element 完全一致)
  if (current == nullptr || current->get_key() != key) {
      int random_level = get_random_level();
      
      if (random_level > _skip_list_level) {
          for (int i = _skip_list_level + 1; i < random_level + 1; i++) {
              update[i] = _header;
          }
          _skip_list_level = random_level;
      }

      Node<K, V> *inserted_node = create_node(key, value, random_level);

      for (int i = 0; i <= random_level; i++) {
          inserted_node->forward[i] = update[i]->forward[i];
          update[i]->forward[i] = inserted_node;
      }
      _element_count++;
  }
  
  // lock 析构自动解锁
}

// Search for element in skip list
/*
                           +------------+
                           |  select 60 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |
level 3         1+-------->10+------------------>50+           70       100
                                                   |
                                                   |
level 2         1          10         30         50|           70       100
                                                   |
                                                   |
level 1         1    4     10         30         50|           70       100
                                                   |
                                                   |
level 0         1    4   9 10         30   40    50+-->60      70       100
*/
template <typename K, typename V>
bool SkipList<K, V>::search_element(const K& key, V &value) {
  
  // 1. (改进 - 关键) 使用共享锁 (读锁)
  //    允许多个线程同时进入此函数进行查找，互不阻塞。
  //    但如果有线程持有独占锁(正在写)，这里会等待。
  std::shared_lock<std::shared_mutex> lock(_mtx);

  // 2. (改进 - 性能) 移除 std::cout
  //    高频调用的查找函数中绝对不能有 I/O 操作
  // std::cout << "search_element-----------------" << std::endl;

  Node<K, V> *current = _header;

  // 3. 从顶层向下查找
  for (int i = _skip_list_level; i >= 0; i--) {
    while (current->forward[i] && current->forward[i]->get_key() < key) {
      current = current->forward[i];
    }
  }

  current = current->forward[0]; // 移动到第0层的目标节点

  // 4. (改进 - 风格) 使用 && 替代 and
  if (current && current->get_key() == key) {
    value = current->get_value();
    // std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
    return true;
  }

  return false;
}

template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node) {
  keyDumpVt_.emplace_back(node.get_key());
  valDumpVt_.emplace_back(node.get_value());
}

// construct skip list
template <typename K, typename V>
SkipList<K, V>::SkipList(int max_level) 
    :_max_level(max_level),
    _skip_list_level(0),
    _element_count(0) {

    // 创建哨兵节点 (Header)
    // 注意：K() 和 V() 确保调用键值的默认构造函数
    K k = K(); 
    V v = V();
    this->_header = new Node<K, V>(k, v, _max_level);
}


template <typename K, typename V>
SkipList<K, V>::~SkipList() {
  // 1. 文件流会自动关闭，无需手动 close

  // 2. (改进 - 关键) 迭代删除所有节点，防止栈溢出
  // 从 header 的第 0 层（最底层，包含所有节点）开始遍历
  Node<K, V> *current = _header->forward[0];
  
  while (current != nullptr) {
      Node<K, V> *next = current->forward[0]; // 先保存下一个节点
      delete current;                         // 删除当前节点
      current = next;                         // 移动指针
  }

  // 3. 最后删除头节点
  delete _header;
}

// 迭代版本的 clear，安全且高效，供析构函数和 load_file 复用。
template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> * /*unused*/) {
    // 注意：这里的参数其实没用了，因为我们总是从 _header->forward[0] 开始删。
    // 为了接口兼容，或者你可以重构这个函数不带参数。
    
    Node<K, V> *current = _header->forward[0];
    while (current != nullptr) {
        Node<K, V> *next = current->forward[0];
        delete current;
        current = next;
    }
    
    // 重置 header 指针，防止悬空指针
    memset(_header->forward, 0, sizeof(Node<K, V> *) * (_max_level + 1));
    _element_count = 0;
    _skip_list_level = 0;
}

template <typename K, typename V>
int SkipList<K, V>::get_random_level() {
  static thread_local std::mt19937 generator(std::random_device{}());
  static thread_local std::uniform_int_distribution<int> distribution(0, 1);

  int k = 1;
  while (distribution(generator) % 2) {
      k++;
  }
  k = (k < _max_level) ? k : _max_level;
  return k;
}
// vim: et tw=100 ts=4 sw=4 cc=120
#endif  // SKIPLIST_