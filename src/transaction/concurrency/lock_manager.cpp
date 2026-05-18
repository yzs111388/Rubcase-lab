/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "lock_manager.h"

bool LockManager::mode_covers(LockMode held, LockMode requested) const {
    if (held == requested) return true;
    if (held == LockMode::EXLUCSIVE) return true;
    if (held == LockMode::S_IX) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED ||
               requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) return requested == LockMode::INTENTION_SHARED;
    if (held == LockMode::INTENTION_EXCLUSIVE) return requested == LockMode::INTENTION_SHARED;
    return false;
}

bool LockManager::is_compatible(LockMode held, LockMode requested) const {
    if (held == LockMode::INTENTION_SHARED) {
        return requested != LockMode::EXLUCSIVE;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED || requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::INTENTION_SHARED || requested == LockMode::SHARED;
    }
    if (held == LockMode::S_IX) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;  // X conflicts with every other transaction.
}

LockManager::GroupLockMode LockManager::compute_group_lock_mode(const LockRequestQueue &queue) const {
    bool has_s = false, has_x = false, has_is = false, has_ix = false, has_six = false;
    for (auto &req : queue.request_queue_) {
        if (!req.granted_) continue;
        switch (req.lock_mode_) {
            case LockMode::SHARED: has_s = true; break;
            case LockMode::EXLUCSIVE: has_x = true; break;
            case LockMode::INTENTION_SHARED: has_is = true; break;
            case LockMode::INTENTION_EXCLUSIVE: has_ix = true; break;
            case LockMode::S_IX: has_six = true; break;
        }
    }
    if (has_x) return GroupLockMode::X;
    if (has_six || (has_s && has_ix)) return GroupLockMode::SIX;
    if (has_s) return GroupLockMode::S;
    if (has_ix) return GroupLockMode::IX;
    if (has_is) return GroupLockMode::IS;
    return GroupLockMode::NON_LOCK;
}

bool LockManager::lock(Transaction *txn, const LockDataId &lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) return true;
    std::unique_lock<std::mutex> lk(latch_);
    if (txn->get_state() == TransactionState::SHRINKING) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    auto &queue = lock_table_[lock_data_id];
    auto self = queue.request_queue_.end();
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            self = it;
            break;
        }
    }
    if (self != queue.request_queue_.end() && self->granted_ && mode_covers(self->lock_mode_, lock_mode)) {
        return true;
    }

    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (!it->granted_ || it->txn_id_ == txn->get_transaction_id()) continue;
        if (!is_compatible(it->lock_mode_, lock_mode) || !is_compatible(lock_mode, it->lock_mode_)) {
            txn->set_state(TransactionState::ABORTED);
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    if (self == queue.request_queue_.end()) {
        queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
        self = std::prev(queue.request_queue_.end());
    } else {
        self->lock_mode_ = lock_mode;
    }
    self->granted_ = true;
    queue.group_lock_mode_ = compute_group_lock_mode(queue);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IS_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IX_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;
    std::unique_lock<std::mutex> lk(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) return false;
    auto &queue = table_it->second;
    bool found = false;
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            queue.request_queue_.erase(it);
            found = true;
            break;
        }
    }
    if (!found) return false;
    txn->get_lock_set()->erase(lock_data_id);
    if (txn->get_state() == TransactionState::GROWING) {
        txn->set_state(TransactionState::SHRINKING);
    }
    queue.group_lock_mode_ = compute_group_lock_mode(queue);
    queue.cv_.notify_all();
    if (queue.request_queue_.empty()) {
        lock_table_.erase(table_it);
    }
    return true;
}
