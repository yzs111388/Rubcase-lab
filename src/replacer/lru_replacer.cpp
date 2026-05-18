/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::victim(frame_id_t* frame_id) {
    std::scoped_lock lock{latch_};
    if (LRUlist_.empty()) {
        return false;
    }
    *frame_id = LRUlist_.back();
    LRUhash_.erase(*frame_id);
    LRUlist_.pop_back();
    return true;
}

void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    auto it = LRUhash_.find(frame_id);
    if (it == LRUhash_.end()) {
        return;
    }
    LRUlist_.erase(it->second);
    LRUhash_.erase(it);
}

void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    if (LRUhash_.find(frame_id) != LRUhash_.end()) {
        return;
    }
    if (LRUlist_.size() >= max_size_) {
        return;
    }
    LRUlist_.push_front(frame_id);
    LRUhash_[frame_id] = LRUlist_.begin();
}

size_t LRUReplacer::Size() {
    std::scoped_lock lock{latch_};
    return LRUlist_.size();
}
