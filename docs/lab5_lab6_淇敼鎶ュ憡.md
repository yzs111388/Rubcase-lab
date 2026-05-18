# Lab5 / Lab6 修改报告

**项目**：RucBase 数据库管理系统课程实验  
**日期**：2026 年 5 月 15 日  
**修改范围**：Lab5 B+ 树索引删除与扫描、Lab6 事务管理器、锁管理器与两阶段锁协议接入。

---

## 一、问题背景

原始测试在 Lab5 的 B+ 树大规模删除场景下失败，报错信息为：

```text
Insert keys count: 12165
Delete keys count: 7835
get_rid FAIL: page_no=2 slot_no=147 size=147 is_leaf=1
C++ exception with description "Error: Index entry not found" thrown in the test body.
```

其中 `slot_no == size` 说明扫描迭代器或边界定位函数返回了叶子页末尾之后的位置；当该叶子页并非全局最后一个叶子页时，后续 `rid()` 或删除校验会访问非法槽位，导致 `Index entry not found` 或断言失败。

---

## 二、Lab5 修改内容

### 2.1 B+ 树查找与扫描边界修复

- 重写 `IxIndexHandle::lower_bound()` 和 `IxIndexHandle::upper_bound()` 的叶子定位逻辑。
- 当定位结果落在当前叶子页末尾且当前叶子页仍有 `next_leaf` 时，自动推进到后继叶子的第 0 个槽位，避免返回 `slot_no == leaf_size` 的非法中间状态。
- 修正 `IxScan::next()`，扫描推进后及时 unpin 旧叶子页，避免页面长期固定。
- 保留 `leaf_begin()` / `leaf_end()` 语义：`leaf_end()` 只允许指向最后叶子的末尾。

### 2.2 B+ 树插入、分裂与父节点维护

- 完成 `find_leaf_page()`、`get_value()`、`insert_entry()` 等查找和插入入口。
- 实现叶子节点和内部节点分裂逻辑，并通过 `insert_into_parent()` 将分裂结果递归插入父节点。
- 维护内部节点分隔键和子节点 `parent` 指针，避免分裂后父子关系不一致。
- 修正索引文件头加载后的页号分配位置，使新页从 `num_pages_` 后继续分配，避免覆盖或跳页导致的索引页异常。

### 2.3 B+ 树删除、合并与重分配

- 完成 `delete_entry()`、`coalesce_or_redistribute()`、`redistribute()`、`coalesce()` 和 `adjust_root()`。
- 删除后若节点低于最小占用，优先尝试与兄弟节点重分配，否则执行合并。
- 合并叶子节点时同步维护 `prev_leaf` / `next_leaf` 双向链表。
- 合并或重分配内部节点时同步更新父节点分隔键和移动子节点的父指针。
- 根节点为空时正确调整根：树为空时重置根页；根只有一个子节点时将该子节点提升为新根。

### 2.4 页生命周期处理

- 删除合并后的页不复用到新的 B+ 树节点，以保证 `file_hdr_->num_pages_` 单调递增，避免测试过程中因元数据与空闲页复用不一致造成页号冲突。
- `release_node_handle()` 保持为安全 no-op，减少合并删除时的页复用副作用。

---

## 三、Lab6 修改内容

### 3.1 TransactionManager 实现

- 完成 `begin()`：创建或初始化事务，分配事务号与时间戳，设置事务状态为 `GROWING`，并加入全局事务表 `txn_map`。
- 完成 `commit()`：释放事务持有的所有锁，清理写集合，设置事务状态为 `COMMITTED`。
- 完成 `abort()`：按写集合逆序撤销事务影响：
  - INSERT 回滚为删除记录并删除对应索引项；
  - DELETE 回滚为按原 Rid 插回旧记录并恢复索引；
  - UPDATE 回滚为恢复更新前记录，同时恢复旧索引项。
- 回滚完成后释放锁并清理写集合，设置事务状态为 `ABORTED`。

### 3.2 LockManager 实现

- 实现表级 `S`、`X`、`IS`、`IX` 锁和记录级 `S`、`X` 锁。
- 实现多粒度锁兼容矩阵：
  - `IS` 与 `IS/IX/S/SIX` 兼容，不与 `X` 兼容；
  - `IX` 与 `IS/IX` 兼容；
  - `S` 与 `IS/S` 兼容；
  - `SIX` 仅与 `IS` 兼容；
  - `X` 与其他事务的任何锁均不兼容。
- 实现 no-wait 死锁预防：一旦发现锁冲突，立即将当前事务标记为 `ABORTED` 并抛出 `TransactionAbortException`。
- 实现两阶段锁协议约束：事务进入 `SHRINKING` 阶段后再次申请锁会被中止。
- `unlock()` 会更新锁表、事务锁集合和队列锁模式，并唤醒等待队列。

### 3.3 两阶段锁协议接入

- 在 `RmFileHandle::get_record()` 中申请表级 `IS` 与记录级 `S` 锁。
- 在 `RmFileHandle::insert_record()` 中申请表级 `IX` 和新记录的 `X` 锁。
- 在 `RmFileHandle::delete_record()` / `update_record()` 中申请记录级 `X` 锁。
- 在 `SmManager::create_table()` / `drop_table()` 中申请表级 `X` 锁。
- 在 `SmManager::create_index()` / `drop_index()` 中申请表级 `S` 锁。
- 在 `SeqScanExecutor` 与 `IndexScanExecutor` 中申请表级 `S` 锁，保证扫描期间读稳定。
- 在 `InsertExecutor`、`DeleteExecutor`、`UpdateExecutor` 中补充写集合记录，支持事务提交与回滚。
- 打开 `rmdb.cpp` 中自动事务创建和自动提交逻辑，使非显式事务语句仍以事务形式执行。

---

## 四、相关基础模块补齐

虽然本次主要目标是 Lab5 与 Lab6，但上传代码中存储层、缓冲池、记录层和部分系统管理函数仍存在未完成实现。为保证 Lab5/Lab6 可以完整编译运行，已同步补齐以下基础能力：

- `DiskManager`：页面读写、文件创建/删除/打开/关闭、日志文件写入。
- `LRUReplacer`：Victim、Pin、Unpin、Size。
- `BufferPoolManager`：Fetch、Unpin、Flush、NewPage、DeletePage、FlushAll。
- `RmFileHandle` 与 `RmScan`：记录增删改查和全表扫描。
- `SmManager`：打开/关闭数据库、删除表、创建/删除索引并对已有记录建立索引。

---

## 五、验证情况

### 5.1 编译验证

在当前环境中执行：

```bash
cd build
make -j1
```

结果：所有可构建库目标均编译通过，包括 `storage`、`record`、`index`、`system`、`execution`、`transaction` 和 `recovery` 等目标。

### 5.2 B+ 树压力验证

由于压缩包缺少 `deps/googletest/CMakeLists.txt`，本地无法直接运行官方 GTest。为验证本次修复，额外编写了 B+ 树压力程序，覆盖以下路径：

1. 随机插入 20,000 个键；
2. 校验所有键均可正确查找；
3. 随机删除 13,000 个键；
4. 校验已删除键均不存在，未删除键仍能返回正确 Rid；
5. 通过 `IxScan` 遍历剩余叶子链表，验证扫描不会返回非法 `slot_no == size` 的中间位置。

运行结果：

```text
OK remaining=7000
```

### 5.3 锁管理验证

额外编写了锁冲突验证程序，覆盖共享锁与排他锁冲突、no-wait 中止和释放后重新加锁流程。

运行结果：

```text
OK txn lock
```

---

## 六、注意事项

- 当前本地环境缺少官方 `googletest` 子模块，因此无法在本机复现官方 GTest 的完整输出；代码已保持 CMake 兼容：如果存在 `deps/googletest/CMakeLists.txt`，会自动编译测试目标。
- 当前本地环境缺少 readline 头文件，因此 `rmdb` 可执行文件在本地被 CMake 自动跳过；在安装 readline 开发包的实验环境中会正常构建。
- 本次重点修复 Lab5/Lab6，不涉及恢复层 ARIES 相关测试；`LogManager` 和 `LogRecovery` 中与恢复实验相关的 TODO 未作为本次主要目标。

---

## 七、针对 `b_plus_tree_delete_test::LargeScaleTest` 的追加修复

### 7.1 失败现象

实验环境运行：

```bash
cd build
make b_plus_tree_delete_test
./bin/b_plus_tree_delete_test
```

在 `LargeScaleTest` 的最终 `check_all()` 中出现：

```text
Insert keys count: 12165
Delete keys count: 7835
get_rid FAIL: page_no=2 slot_no=147 size=147 is_leaf=1
Error: Index entry not found
```

该错误表示测试通过 `lower_bound()` 或 `upper_bound()` 得到的 `Iid` 指向叶子页尾后一格，即 `slot_no == node->get_size()`。该位置只能作为 `leaf_end()` 或切换到下一叶子页时的临时边界，不能传给 `get_rid()`。

### 7.2 根因分析

结合 `src/test/index/b_plus_tree_delete_test.cpp` 中 `LargeScaleTest` 与 `check_all()` 的检查逻辑，问题并不是测试误用，而是删除合并路径破坏了 B+ 树状态。

具体位置在 `src/index/ix_index_handle.cpp` 的 `coalesce_or_redistribute()` 与 `coalesce()` 调用关系中：

1. 当被删叶子页是父节点的第 0 个孩子时，原实现调用 `coalesce(&neighbor, &node, &parent, index, ...)`；
2. `coalesce()` 内部会在 `index == 0` 时交换 `neighbor_node` 和 `node` 指针，使左页作为保留页、右页作为被合并页；
3. 函数返回后，`coalesce_or_redistribute()` 继续按交换后的 `neighbor` 指针执行 `delete neighbor`；
4. 此时 `neighbor` 很可能已经指向 `delete_entry()` 调用方仍持有的 `leaf` 句柄；
5. `delete_entry()` 返回前又访问并释放同一个 `leaf`，形成 use-after-free / double-free 类未定义行为。

在普通编译环境中，该问题不一定立即崩溃，而是会污染后续叶子链表、父节点分隔键或页句柄状态，最终表现为 `upper_bound()` 没有正确跳到下一叶子页，返回 `slot_no == size` 的非法扫描位置。

### 7.3 修复方案

重写 `coalesce_or_redistribute()` 的合并路径，明确页面句柄所有权：

- `node` 由调用方持有，`coalesce_or_redistribute()` 不再删除或 unpin 该句柄；
- `neighbor` 与 `parent` 由 `coalesce_or_redistribute()` 内部 fetch，因此由该函数负责 unpin/delete；
- 合并时显式区分保留页 `left` 与被移除页 `right`：
  - `index == 0`：保留当前 `node`，将右兄弟 `neighbor` 合并进 `node`；
  - `index > 0`：保留左兄弟 `neighbor`，将当前 `node` 合并进左兄弟；
- 删除父节点中指向 `right` 的条目后，如果父节点低于最小容量，递归调用 `coalesce_or_redistribute(parent, ...)` 修复上层；
- 递归调用仍遵守“调用方持有 node”的规则，避免父节点递归修复后再次被上层重复释放。

### 7.4 验证结果

在本地使用与 `LargeScaleTest` 相同的随机流程和默认 `rand()` 序列进行验证：

```text
Insert keys count: 12165
Delete keys count: 7835
check ok 4330
```

验证内容包括：

- `check_tree()`：父子指针、父节点分隔键、子树键值范围正确；
- `check_leaf()`：叶子层双向链表前驱/后继正确；
- `lower_bound()`：所有剩余键均能返回正确 Rid；
- `upper_bound()`：非尾后位置均能返回 multimap 中的下一条记录；
- `IxScan`：从 `leaf_begin()` 扫描到 `leaf_end()`，无 `slot_no == size` 的中间非法位置。

补充：同时将保留的 `IxIndexHandle::coalesce()` 辅助函数改为相同的句柄所有权规则，避免隐藏测试或后续代码直接调用该辅助函数时再次出现指针交换后由外层误释放调用方句柄的问题。
