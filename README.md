# Distributed-Key-Value-Store
本项目是一个基于 Raft 共识算法 实现的分布式键值存储系统（Distributed Key-Value Store）。 系统通过 Raft 算法实现节点间的一致性同步，保证在多数节点正常的情况下，系统仍能保持强一致性和高可用性。  项目模块化设计，包含：  Common 模块：基础工具与日志封装；  SkipList 模块：高效有序数据存储结构；  Raft 核心模块：实现 Leader 选举、日志复制、日志提交等核心机制；  RPC 模块：节点间通信封装，支持客户端请求与节点内部同步；
