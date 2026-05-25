/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"
#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0;
    if (!page_hdr->is_leaf) { // 如果是内部节点，从 1 开始
        left = 1; 
    }
    int right = page_hdr->num_key;
    while (left < right) {
        int mid = (left + right) / 2;
        int flag = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (flag < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    // Fix: Leaf nodes should start searching from 0, Internal nodes from 1
    int left = 1;
    if (page_hdr->is_leaf) {
        left = 0;
    }
    int right = page_hdr->num_key;
    while (left < right) {
        int mid = (left + right) / 2;
        int flag = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (flag > 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < get_size()) {
        // Fix: Must check if the key found is actually EQUAL to the target key
        if (ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            *value = get_rid(pos);
            return true;
        }
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = lower_bound(key);
    if (pos == get_size()) {
        return value_at(pos - 1);
    }
    int cmp = ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_);
    if (cmp == 0) {
        return value_at(pos);     // 修复: 相等时去当前位置 (取决于你的lower_bound语义，通常>=去右边，这里保持pos即可)
    } else {
        return value_at(pos - 1); // 修复: 小于时去左边 (pos - 1)
    }
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 * key_slot
 * /      \
 * /        \
 * [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 * key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos <= get_size() && pos >= 0);
    int num = page_hdr->num_key - pos;
    char *begin_key = get_key(pos);
    Rid *begin_rid = get_rid(pos);
    
    int key_len = file_hdr->col_lens_[0];
    int tot_len = file_hdr->col_tot_len_;

    memmove(begin_key + n * tot_len, begin_key, num * tot_len);
    memcpy(begin_key, key, n * tot_len);
    
    memmove(begin_rid + n, begin_rid, num * sizeof(Rid));
    memcpy(begin_rid, rid, n * sizeof(Rid));
    
    set_size(get_size() + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    int flag = 1;
    if (pos < get_size()) {
        flag = ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_);
    }

    if (pos == get_size() || flag > 0) {
        insert_pair(pos, key, value);
    }
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    assert(pos < get_size() && pos >= 0);
    int num = page_hdr->num_key - pos - 1;
    int tot_len = file_hdr->col_tot_len_;
    
    char *key = get_key(pos);
    Rid *rid = get_rid(pos);
    
    memmove(key, key + tot_len, num * tot_len);
    memmove(rid, rid + 1, num * sizeof(Rid));
    
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < get_size()) {
        int flag = ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_);
        if (flag == 0) {
            erase_pair(pos);
        }
    }
    return get_size();
}



IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char *buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf; // Fix: Memory leak
    
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    IxNodeHandle *cur = fetch_node(file_hdr_->root_page_); // Start at root
    while (!cur->is_leaf_page()) {
      page_id_t next_page_id = cur->internal_lookup(key);
        
        // 注意：这里需要释放当前节点
        IxNodeHandle *next = fetch_node(next_page_id);
        buffer_pool_manager_->unpin_page(cur->get_page_id(), false);
        delete cur;
        
        cur = next;
    }
    return std::make_pair(cur, false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, _] = find_leaf_page(key, Operation::FIND, transaction);
    
    Rid *rid = nullptr;
    bool found = false;
    if (leaf->leaf_lookup(key, &rid)) {
        result->push_back(*rid);
        found = true;
    }
    
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf; // Fix: Memory leak
    return found;
}

/**
 * @brief 将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->parent = node->get_parent_page_no();
    new_node->page_hdr->next_free_page_no = node->page_hdr->next_free_page_no;

    if (new_node->is_leaf_page()) {
        new_node->page_hdr->prev_leaf = node->get_page_no();
        new_node->page_hdr->next_leaf = node->get_next_leaf();
        node->page_hdr->next_leaf = new_node->get_page_no();

        if (new_node->page_hdr->next_leaf != INVALID_PAGE_ID) {
            IxNodeHandle *next_node = fetch_node(new_node->page_hdr->next_leaf);
            next_node->page_hdr->prev_leaf = new_node->get_page_no();
            buffer_pool_manager_->unpin_page(next_node->get_page_id(), true);
            delete next_node; // Fix: Memory leak
        }
    }

    int pos;
    if (node->is_leaf_page()) {
        pos = (node->get_size() + 1) / 2;  // 叶子节点通常取一半
    } else {
        pos = node->get_size() / 2;        // 内部节点取中间位置
    }
    int num = node->get_size() - pos;
    
    // Move keys to new node
    new_node->insert_pairs(0, node->get_key(pos), node->get_rid(pos), num);
    node->set_size(pos);

    // If internal node, update children's parent pointers
    if (!new_node->is_leaf_page()) {
        for (int i = 0; i < num; ++i) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        IxNodeHandle *new_root = create_node();
        new_root->set_size(0);
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->parent = INVALID_PAGE_ID;
        new_root->page_hdr->next_free_page_no = IX_NO_PAGE;

        Rid old_rid, new_rid;
        old_rid.page_no = old_node->get_page_no();
        old_rid.slot_no = -1;
        new_rid.page_no = new_node->get_page_no();
        new_rid.slot_no = -1;

        new_root->insert_pair(0, old_node->get_key(0), old_rid);
        new_root->insert_pair(1, key, new_rid);

        file_hdr_->root_page_ = new_root->get_page_no();
        new_node->set_parent_page_no(new_root->get_page_no());
        old_node->set_parent_page_no(new_root->get_page_no());
        
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        delete new_root; // Fix: Memory leak

    } else {
        Rid new_rid;
        new_rid.page_no = new_node->get_page_id().page_no;
        new_rid.slot_no = -1;

        IxNodeHandle *parent_node = fetch_node(old_node->get_parent_page_no());
        int rid_index = parent_node->find_child(old_node);
        
        parent_node->insert_pair(rid_index + 1, key, new_rid);

        if (parent_node->get_size() == parent_node->get_max_size()) {
            IxNodeHandle *new_parent_node = split(parent_node);
            insert_into_parent(parent_node, new_parent_node->get_key(0), new_parent_node, transaction);
            buffer_pool_manager_->unpin_page(new_parent_node->get_page_id(), true);
            delete new_parent_node;
        }
        
        buffer_pool_manager_->unpin_page(parent_node->get_page_id(), true);
        delete parent_node;
    }
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
bool IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf_node, _] = find_leaf_page(key, Operation::INSERT, transaction);
    
    int cur_size = leaf_node->get_size();
    if (leaf_node->insert(key, value) == cur_size) {
        // Duplicate key
        buffer_pool_manager_->unpin_page(leaf_node->get_page_id(), false);
        delete leaf_node;
        return false;
    }

    if (leaf_node->get_size() == leaf_node->get_max_size()) {
        IxNodeHandle *new_node = split(leaf_node);
        if (leaf_node->get_page_no() == file_hdr_->last_leaf_) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
        // Leaf split: copy the separator key up
        insert_into_parent(leaf_node, new_node->get_key(0), new_node, transaction);
        
        buffer_pool_manager_->unpin_page(new_node->get_page_id(), true);
        delete new_node;
    }

    buffer_pool_manager_->unpin_page(leaf_node->get_page_id(), true); // Dirty because we inserted
    delete leaf_node;
    return true;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, _] = find_leaf_page(key, Operation::DELETE, transaction);
    
    int size = leaf->get_size();
    if (leaf->remove(key) == size) {
        // Not found
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return false;
    }

    coalesce_or_redistribute(leaf, transaction);
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->get_size() >= node->get_min_size()) {
        if (!node->is_root_page()) {
            maintain_parent(node);
        }
        return false;  // 不需要删除节点
    }
    
    // 如果是根节点，特殊处理
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    
    // 获取父节点和兄弟节点
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);
    
    IxNodeHandle *sibling = nullptr;
    bool is_left_sibling = true;
    
    // 选择要借用的兄弟节点（优先用左兄弟）
    if (index == 0) {
        sibling = fetch_node(parent->value_at(1));  // 右兄弟
        is_left_sibling = false;
    } else {
        sibling = fetch_node(parent->value_at(index - 1));  // 左兄弟
    }
    
    // 判断是重新分配还是合并
    if (node->get_size() + sibling->get_size() >= node->get_max_size()) {
        // 可以重新分配
        redistribute(sibling, node, parent, index);
        
        // unpin页面
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        buffer_pool_manager_->unpin_page(sibling->get_page_id(), true);
        delete parent;
        delete sibling;
        return false;  // 不需要删除节点
    } else {
        // 需要合并
        bool parent_needs_deletion = coalesce(&sibling, &node, &parent, 
                                             index, 
                                             transaction, root_is_latched);
        
        // 注意：coalesce中已经处理了节点的unpin和删除
        return parent_needs_deletion;
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (old_root_node->is_leaf_page()) {
        if (old_root_node->get_size() == 0) {
            // Tree is empty
            release_node_handle(*old_root_node);
            file_hdr_->root_page_ = INVALID_PAGE_ID;
            return true;
        }
    } else if (old_root_node->get_size() == 1) {
        // Internal node with only one child -> promote child to root
        IxNodeHandle *new_root = fetch_node(old_root_node->value_at(0));
        new_root->set_parent_page_no(INVALID_PAGE_ID);
        file_hdr_->root_page_ = new_root->get_page_no();
        
        release_node_handle(*old_root_node);
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        delete new_root;
        return true;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index != 0) {
        // --- 情况 1: neighbor 在左，node 在右 (Borrow from Left) ---
        // node 借用 neighbor 的最后一个元素
        int borrow_idx = neighbor_node->get_size() - 1;
        
        // 1. 将邻居节点的最后一个键值对插入到node头部
        // 注意：无论是叶子还是内部节点，都直接移动(key, value)，因为内部节点的key就是对应child的min_key
        node->insert_pair(0, neighbor_node->get_key(borrow_idx), *neighbor_node->get_rid(borrow_idx));
        
        // 2. 删除邻居节点的最后一个元素
        neighbor_node->erase_pair(borrow_idx);

        // 3. 更新父节点key (node的min key变了，为刚刚移过来的key)
        // node在index位置
        memcpy(parent->get_key(index), node->get_key(0), file_hdr_->col_tot_len_);

        // 4. 如果是内部节点，更新孩子节点的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    } else {  // node在左边，从右边借 (Borrow from Right)
        // 从右边兄弟借第一个元素
        
        // 1. 将邻居节点的第一个键值对插入到node尾部
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        
        // 2. 删除邻居节点的第一个元素
        neighbor_node->erase_pair(0);
        
        // 3. 更新父节点key (neighbor的min key变了)
        // neighbor在index+1位置
        memcpy(parent->get_key(index + 1), neighbor_node->get_key(0), file_hdr_->col_tot_len_);

        // 4. 如果是内部节点，更新孩子节点的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                           Transaction *transaction, bool *root_is_latched) {
    // If index == 0, node is Left, neighbor is Right.
    // We want neighbor to be Left, node to be Right.
    if (index == 0) {
        IxNodeHandle *tmp = *neighbor_node;
        *neighbor_node = *node;
        *node = tmp;
        index++; 
    }

    // REMOVED: Incorrect overwriting of node->key[0] with parent->key[index].
    // In Min-Key B+ Tree, node->key[0] is already the valid min key of the child.
    // Overwriting it with parent key is redundant at best, and corrupting at worst (if parent key is stale).


    int before_num = (*neighbor_node)->get_size();
    (*neighbor_node)->insert_pairs(before_num, (*node)->get_key(0), (*node)->get_rid(0), (*node)->get_size());
    int after_num = (*neighbor_node)->get_size();

    for (int i = before_num; i < after_num; i++) {
        maintain_child(*neighbor_node, i);
    }

    if ((*node)->is_leaf_page()) {
        if ((*node)->get_page_no() == file_hdr_->last_leaf_) {
            file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
        }
        erase_leaf(*node);
    }

    release_node_handle(**node);

    (*parent)->erase_pair(index);
    bool parent_needs_deletion = coalesce_or_redistribute(*parent, transaction, root_is_latched);
    
    // 如果父节点需要删除，返回true，否则返回false（节点已合并）
    return parent_needs_deletion;  
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    Rid rid = *node->get_rid(iid.slot_no);
    delete node; // Fix: Memory leak
    return rid;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    if (root_is_latched) {
        root_latch_.unlock();
    }

    if (!leaf) {
        return Iid{-1, -1};
    }

    int slot_no = leaf->lower_bound(key);
    
    // Fix: If slot_no reached the end of the node, and it's not the last leaf,
    // we should move to the beginning of the next leaf node.
    // NOTE: The terminator is IX_LEAF_HEADER_PAGE, not just INVALID_PAGE_ID.
    if (slot_no == leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE && leaf->get_next_leaf() != IX_NO_PAGE) {
        page_id_t next_page = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return Iid{.page_no = next_page, .slot_no = 0};
    }

    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = slot_no};

    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf; 
    return iid;
}

Iid IxIndexHandle::upper_bound(const char *key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    if (root_is_latched) {
        root_latch_.unlock();
    }

    if (!leaf) {
        return Iid{-1, -1};
    }

    int slot_no = leaf->upper_bound(key);

    // Fix: If slot_no reached the end of the node, and it's not the last leaf,
    // we should move to the beginning of the next leaf node.
    // NOTE: The terminator is IX_LEAF_HEADER_PAGE, not just INVALID_PAGE_ID.
    if (slot_no == leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE && leaf->get_next_leaf() != IX_NO_PAGE) {
        page_id_t next_page = leaf->get_next_leaf();
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return Iid{.page_no = next_page, .slot_no = 0};
    }

    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = slot_no};

    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf; 
    return iid;
}

Iid IxIndexHandle::leaf_end() const {
    // Traverse from root to the rightmost leaf. Do not rely on last_leaf_ being correct.
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        int sz = node->get_size();
        page_id_t child_page_no = node->value_at(sz > 0 ? (sz - 1) : 0);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        node = fetch_node(child_page_no);
    }

    // Move to the last non-empty leaf if needed.
    while (node->get_size() == 0 && node->get_prev_leaf() != IX_NO_PAGE) {
        page_id_t prev = node->get_prev_leaf();
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        node = fetch_node(prev);
    }

    Iid iid = {.page_no = node->get_page_no(), .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
    return iid;
}

Iid IxIndexHandle::leaf_begin() const {
    // Traverse from root to the leftmost leaf. Do not rely on first_leaf_ being correct.
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        page_id_t child_page_no = node->value_at(0);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        node = fetch_node(child_page_no);
    }

    // Skip empty leaves if any.
    while (node->get_size() == 0 && node->get_next_leaf() != IX_NO_PAGE) {
        page_id_t next = node->get_next_leaf();
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        node = fetch_node(next);
    }

    Iid iid = {.page_no = node->get_page_no(), .slot_no = 0};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
    return iid;
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    return new IxNodeHandle(file_hdr_, page);
}

IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            delete parent;
            break;
        }
        
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);
  
        if (curr != node) {
             delete curr;
        }
        curr = parent;
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    }
    if (curr != node) {
        delete curr;
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());
    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    delete prev; // Fix: Memory leak

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    delete next; // Fix: Memory leak
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child; // Fix: Memory leak
    }
}