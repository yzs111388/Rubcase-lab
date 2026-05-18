/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "buffer_pool_manager.h"

bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Page *page = &pages_[it->second];
        page->pin_count_++;
        replacer_->pin(it->second);
        return page;
    }

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    Page *page = &pages_[frame_id];
    if (page->id_.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        }
        page_table_.erase(page->id_);
    }
    page->reset_memory();
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->id_ = page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    page_table_[page_id] = frame_id;
    return page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    if (page->pin_count_ <= 0) {
        return false;
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(it->second);
    }
    return true;
}

bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page *page = &pages_[it->second];
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    Page *page = &pages_[frame_id];
    if (page->id_.page_no != INVALID_PAGE_ID) {
        if (page->is_dirty_) {
            disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        }
        page_table_.erase(page->id_);
    }

    page_id->page_no = disk_manager_->allocate_page(page_id->fd);
    page->reset_memory();
    page->id_ = *page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    page_table_[*page_id] = frame_id;
    return page;
}

bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    if (page->pin_count_ > 0) {
        return false;
    }
    replacer_->pin(frame_id);
    page_table_.erase(it);
    page->reset_memory();
    page->id_ = PageId{-1, INVALID_PAGE_ID};
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    free_list_.push_back(frame_id);
    return true;
}

void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (auto &entry : page_table_) {
        const PageId &pid = entry.first;
        if (pid.fd != fd) {
            continue;
        }
        Page *page = &pages_[entry.second];
        disk_manager_->write_page(pid.fd, pid.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}
