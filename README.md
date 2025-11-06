# Distributed-Key-Value-Store
本项目是一个基于 Raft 共识算法 实现的分布式键值存储系统（Distributed Key-Value Store）。
系统通过 Raft 算法实现节点间的一致性同步，保证在多数节点正常的情况下，系统仍能保持强一致性和高可用性。

项目模块化设计，包含：
Common 模块：基础工具与日志封装；
SkipList 模块：高效有序数据存储结构；
Raft 核心模块：实现 Leader 选举、日志复制、日志提交等核心机制；
RPC 模块：节点间通信封装，支持客户端请求与节点内部同步；
Client & Server：支持基本的 GET / PUT / DELETE 操作。

⚙️ 功能特性
✅ Leader 选举：基于 Raft 超时机制自动选举 Leader；
✅ 日志复制与提交：保证所有节点状态一致；
✅ 持久化存储：日志条目与状态机支持落盘；
✅ 节点容错恢复：崩溃节点重启后可从 Leader 同步数据；
✅ 可扩展结构：采用 SkipList 提高本地存取效率；
✅ RPC 通信：基于自定义 RPC 或 Protobuf 实现跨节点通信。
