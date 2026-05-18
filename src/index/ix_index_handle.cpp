/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "ix_index_handle.h"

#include "ix_scan.h"

int IxNodeHandle::lower_bound(const char *target) const {
    int l = 0, r = page_hdr->num_key;
    while (l < r) {
        int m = (l + r) >> 1;
        if (ix_compare(get_key(m), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            l = m + 1;
        } else {
            r = m;
        }
    }
    return l;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int l = 1, r = page_hdr->num_key;
    while (l < r) {
        int m = (l + r) >> 1;
        if (ix_compare(get_key(m), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            l = m + 1;
        } else {
            r = m;
        }
    }
    return l;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    assert(!is_leaf_page());
    int pos = upper_bound(key) - 1;
    if (pos < 0) pos = 0;
    return value_at(pos);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= get_size());
    assert(n >= 0 && get_size() + n <= get_max_size());
    int old_size = get_size();
    int key_len = file_hdr->col_tot_len_;
    memmove(keys + (pos + n) * key_len, keys + pos * key_len, (old_size - pos) * key_len);
    memmove(rids + pos + n, rids + pos, (old_size - pos) * sizeof(Rid));
    memcpy(keys + pos * key_len, key, n * key_len);
    memcpy(rids + pos, rid, n * sizeof(Rid));
    set_size(old_size + n);
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return -1;
    }
    insert_pair(pos, key, value);
    return get_size();
}

void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < get_size());
    int old_size = get_size();
    int key_len = file_hdr->col_tot_len_;
    memmove(keys + pos * key_len, keys + (pos + 1) * key_len, (old_size - pos - 1) * key_len);
    memmove(rids + pos, rids + pos + 1, (old_size - pos - 1) * sizeof(Rid));
    set_size(old_size - 1);
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos >= get_size() || ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) != 0) {
        return -1;
    }
    erase_pair(pos);
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                              Transaction *transaction, bool find_first) {
    (void)operation;
    (void)transaction;
    if (is_empty()) {
        return std::make_pair(nullptr, false);
    }
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        page_id_t next_page_no = find_first ? node->value_at(0) : node->internal_lookup(key);
        PageId old_page_id = node->get_page_id();
        delete node;
        buffer_pool_manager_->unpin_page(old_page_id, false);
        node = fetch_node(next_page_no);
    }
    return std::make_pair(node, false);
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    if (result != nullptr) result->clear();
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);
    (void)root_is_latched;
    if (leaf == nullptr) {
        return false;
    }
    Rid *rid = nullptr;
    bool found = leaf->leaf_lookup(key, &rid);
    if (found && result != nullptr) {
        result->push_back(*rid);
    }
    PageId page_id = leaf->get_page_id();
    delete leaf;
    buffer_pool_manager_->unpin_page(page_id, false);
    return found;
}

IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    new_node->page_hdr->next_free_page_no = IX_NO_PAGE;
    new_node->page_hdr->parent = node->get_parent_page_no();
    new_node->page_hdr->num_key = 0;
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->page_hdr->prev_leaf = IX_NO_PAGE;
    new_node->page_hdr->next_leaf = IX_NO_PAGE;

    int total = node->get_size();
    int split_pos = total / 2;
    int move_num = total - split_pos;
    new_node->insert_pairs(0, node->get_key(split_pos), node->get_rid(split_pos), move_num);
    node->set_size(split_pos);

    if (node->is_leaf_page()) {
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(node->get_next_leaf());
        if (node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node(node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
            delete next;
        } else {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
        node->set_next_leaf(new_node->get_page_no());
        IxNodeHandle *leaf_header = fetch_node(IX_LEAF_HEADER_PAGE);
        leaf_header->set_prev_leaf(file_hdr_->last_leaf_);
        buffer_pool_manager_->unpin_page(leaf_header->get_page_id(), true);
        delete leaf_header;
    } else {
        for (int i = 0; i < new_node->get_size(); ++i) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction) {
    (void)key;
    if (old_node->is_root_page()) {
        IxNodeHandle *root = create_node();
        root->page_hdr->next_free_page_no = IX_NO_PAGE;
        root->page_hdr->parent = IX_NO_PAGE;
        root->page_hdr->num_key = 0;
        root->page_hdr->is_leaf = false;
        root->page_hdr->prev_leaf = IX_NO_PAGE;
        root->page_hdr->next_leaf = IX_NO_PAGE;
        Rid old_rid{old_node->get_page_no(), -1};
        Rid new_rid{new_node->get_page_no(), -1};
        root->insert_pair(0, old_node->get_key(0), old_rid);
        root->insert_pair(1, new_node->get_key(0), new_rid);
        old_node->set_parent_page_no(root->get_page_no());
        new_node->set_parent_page_no(root->get_page_no());
        update_root_page_no(root->get_page_no());
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        delete root;
        return;
    }

    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    int pos = parent->find_child(old_node) + 1;
    Rid rid{new_node->get_page_no(), -1};
    parent->insert_pair(pos, new_node->get_key(0), rid);
    new_node->set_parent_page_no(parent->get_page_no());

    if (parent->get_size() >= parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
        delete new_parent;
    }
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    delete parent;
}

page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    (void)root_is_latched;
    if (leaf == nullptr) return 0;
    int new_size = leaf->insert(key, value);
    if (new_size == -1) {
        PageId leaf_id = leaf->get_page_id();
        delete leaf;
        buffer_pool_manager_->unpin_page(leaf_id, false);
        return 0;
    }
    page_id_t ret = leaf->get_page_no();
    if (leaf->get_size() >= leaf->get_max_size()) {
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
        delete new_leaf;
    } else if (leaf->get_size() > 0 && ix_compare(leaf->get_key(0), key, file_hdr_->col_types_, file_hdr_->col_lens_) == 0) {
        maintain_parent(leaf);
    }
    PageId leaf_id = leaf->get_page_id();
    delete leaf;
    buffer_pool_manager_->unpin_page(leaf_id, true);
    return ret;
}

bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    (void)root_is_latched;
    if (leaf == nullptr) return false;
    int pos = leaf->lower_bound(key);
    bool first_key_changed = (pos == 0);
    int new_size = leaf->remove(key);
    if (new_size == -1) {
        PageId leaf_id = leaf->get_page_id();
        delete leaf;
        buffer_pool_manager_->unpin_page(leaf_id, false);
        return false;
    }
    if (leaf->get_size() > 0 && first_key_changed) {
        maintain_parent(leaf);
    }
    if (leaf->get_size() < leaf->get_min_size()) {
        coalesce_or_redistribute(leaf, transaction, nullptr);
    }
    PageId leaf_id = leaf->get_page_id();
    delete leaf;
    buffer_pool_manager_->unpin_page(leaf_id, true);
    return true;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    (void)root_is_latched;
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);
    int neighbor_index = (index == 0) ? 1 : index - 1;
    if (neighbor_index < 0 || neighbor_index >= parent->get_size()) {
        PageId parent_id = parent->get_page_id();
        delete parent;
        buffer_pool_manager_->unpin_page(parent_id, false);
        return false;
    }

    IxNodeHandle *neighbor = fetch_node(parent->value_at(neighbor_index));

    // If the two siblings have enough entries in total, borrow one entry from the sibling.
    if (node->get_size() + neighbor->get_size() >= 2 * node->get_min_size()) {
        redistribute(neighbor, node, parent, index);
        PageId neighbor_id = neighbor->get_page_id();
        delete neighbor;
        buffer_pool_manager_->unpin_page(neighbor_id, true);
        PageId parent_id = parent->get_page_id();
        delete parent;
        buffer_pool_manager_->unpin_page(parent_id, true);
        return false;
    }

    // Otherwise merge two siblings.  Keep the left page and remove the right page from the tree.
    // Important: do not delete/unpin `node` here.  The caller owns that handle and will unpin it.
    IxNodeHandle *left = nullptr;
    IxNodeHandle *right = nullptr;
    int parent_remove_index;
    if (index == 0) {
        left = node;
        right = neighbor;
        parent_remove_index = neighbor_index;
    } else {
        left = neighbor;
        right = node;
        parent_remove_index = index;
    }

    int left_old_size = left->get_size();
    if (right->get_size() > 0) {
        left->insert_pairs(left_old_size, right->get_key(0), right->get_rid(0), right->get_size());
    }

    if (left->is_leaf_page()) {
        if (file_hdr_->last_leaf_ == right->get_page_no()) {
            file_hdr_->last_leaf_ = left->get_page_no();
        }
        erase_leaf(right);
    } else {
        for (int i = left_old_size; i < left->get_size(); ++i) {
            maintain_child(left, i);
        }
    }

    parent->erase_pair(parent_remove_index);
    if (parent->get_size() > 0) {
        maintain_parent(parent);
    }
    release_node_handle(*right);

    bool ret = false;
    if (parent->get_size() < parent->get_min_size()) {
        ret = coalesce_or_redistribute(parent, transaction, root_is_latched);
    }

    // neighbor was fetched inside this function.  It may be the kept left page or the removed right page;
    // either way this function, not the caller, owns and unpins that handle.
    PageId neighbor_id = neighbor->get_page_id();
    delete neighbor;
    buffer_pool_manager_->unpin_page(neighbor_id, true);
    PageId parent_id = parent->get_page_id();
    delete parent;
    buffer_pool_manager_->unpin_page(parent_id, true);
    return ret;
}

bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        page_id_t child_page_no = old_root_node->remove_and_return_only_child();
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(IX_NO_PAGE);
        update_root_page_no(child_page_no);
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
        return true;
    }
    // Keep an empty leaf root alive. This preserves the sentinel linked list and permits later inserts.
    return false;
}

void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index == 0) {
        // node is the left sibling; move right sibling's first entry to node's tail.
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        maintain_child(node, node->get_size() - 1);
        int neighbor_parent_index = parent->find_child(neighbor_node);
        parent->set_key(neighbor_parent_index, neighbor_node->get_key(0));
    } else {
        // neighbor_node is the left sibling; move its last entry to node's head.
        int last = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(last), *neighbor_node->get_rid(last));
        neighbor_node->erase_pair(last);
        maintain_child(node, 0);
        int node_parent_index = parent->find_child(node);
        parent->set_key(node_parent_index, node->get_key(0));
    }
}

bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // This helper is kept for compatibility with the lab skeleton.  It follows the same ownership rule as
    // coalesce_or_redistribute(): it only modifies pages and never deletes/unpins handles owned by the caller.
    IxNodeHandle *left = nullptr;
    IxNodeHandle *right = nullptr;
    int parent_remove_index;
    if (index == 0) {
        left = *node;
        right = *neighbor_node;
        parent_remove_index = 1;
    } else {
        left = *neighbor_node;
        right = *node;
        parent_remove_index = index;
    }

    int left_old_size = left->get_size();
    if (right->get_size() > 0) {
        left->insert_pairs(left_old_size, right->get_key(0), right->get_rid(0), right->get_size());
    }
    if (!left->is_leaf_page()) {
        for (int i = left_old_size; i < left->get_size(); ++i) {
            maintain_child(left, i);
        }
    } else {
        if (file_hdr_->last_leaf_ == right->get_page_no()) {
            file_hdr_->last_leaf_ = left->get_page_no();
        }
        erase_leaf(right);
    }

    (*parent)->erase_pair(parent_remove_index);
    if ((*parent)->get_size() > 0) {
        maintain_parent(*parent);
    }
    release_node_handle(*right);
    if ((*parent)->get_size() < (*parent)->get_min_size()) {
        return coalesce_or_redistribute(*parent, transaction, root_is_latched);
    }
    return false;
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        std::cerr << "get_rid FAIL: page_no=" << iid.page_no << " slot_no=" << iid.slot_no
                  << " size=" << node->get_size() << " is_leaf=" << node->is_leaf_page() << std::endl;
        PageId page_id = node->get_page_id();
        delete node;
        buffer_pool_manager_->unpin_page(page_id, false);
        throw IndexEntryNotFoundError();
    }
    Rid rid = *node->get_rid(iid.slot_no);
    PageId page_id = node->get_page_id();
    delete node;
    buffer_pool_manager_->unpin_page(page_id, false);
    return rid;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    auto [node, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    (void)root_is_latched;
    if (node == nullptr) return Iid{-1, -1};
    Iid iid{node->get_page_no(), node->lower_bound(key)};
    if (iid.slot_no == node->get_size() && iid.page_no != file_hdr_->last_leaf_) {
        iid.page_no = node->get_next_leaf();
        iid.slot_no = 0;
    }
    PageId page_id = node->get_page_id();
    delete node;
    buffer_pool_manager_->unpin_page(page_id, false);
    return iid;
}

Iid IxIndexHandle::upper_bound(const char *key) {
    auto [node, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    (void)root_is_latched;
    if (node == nullptr) return Iid{-1, -1};
    Iid iid{node->get_page_no(), node->upper_bound(key)};
    if (iid.slot_no == node->get_size() && iid.page_no != file_hdr_->last_leaf_) {
        iid.page_no = node->get_next_leaf();
        iid.slot_no = 0;
    }
    PageId page_id = node->get_page_id();
    delete node;
    buffer_pool_manager_->unpin_page(page_id, false);
    return iid;
}

Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    PageId page_id = node->get_page_id();
    delete node;
    buffer_pool_manager_->unpin_page(page_id, false);
    return iid;
}

Iid IxIndexHandle::leaf_begin() const {
    return Iid{file_hdr_->first_leaf_, 0};
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        throw InternalError("IxIndexHandle::fetch_node failed");
    }
    return new IxNodeHandle(file_hdr_, page);
}

IxNodeHandle *IxIndexHandle::create_node() {
    file_hdr_->num_pages_++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    if (page == nullptr) {
        throw InternalError("IxIndexHandle::create_node failed");
    }
    return new IxNodeHandle(file_hdr_, page);
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    if (node->get_size() == 0) return;
    page_id_t child_page_no = node->get_page_no();
    page_id_t parent_page_no = node->get_parent_page_no();
    char first_key[IX_MAX_COL_LEN];
    memcpy(first_key, node->get_key(0), file_hdr_->col_tot_len_);

    while (parent_page_no != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(parent_page_no);
        int rank = 0;
        while (rank < parent->get_size() && parent->value_at(rank) != child_page_no) {
            rank++;
        }
        assert(rank < parent->get_size());
        if (memcmp(parent->get_key(rank), first_key, file_hdr_->col_tot_len_) == 0) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), false);
            delete parent;
            break;
        }
        parent->set_key(rank, first_key);
        if (rank != 0) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            delete parent;
            break;
        }
        child_page_no = parent->get_page_no();
        parent_page_no = parent->get_parent_page_no();
        memcpy(first_key, parent->get_key(0), file_hdr_->col_tot_len_);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        delete parent;
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());
    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    delete prev;

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    delete next;
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    (void)node;
    // Physical page recycling is unnecessary for these labs. Keeping num_pages_ monotonic
    // avoids reusing pages that may still be referenced by tests or the buffer pool.
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
    }
}
