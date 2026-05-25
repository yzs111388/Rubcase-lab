/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

/**
 * 辅助函数：判断 S 锁冲突 (No-Wait)
 * 行级 S 锁与 X 锁冲突
 */
bool LockManager::lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    // 1. 检查事务状态
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    // 2. 检查是否已持有锁 (避免重复申请)
    LockDataId lock_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }

    // 3. 检查冲突 (No-Wait 策略)
    // S 锁与 X 锁冲突
    if (queue.group_lock_mode_ == GroupLockMode::X) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    // 4. 授予锁
    txn->get_lock_set()->insert(lock_id);
    txn->set_state(TransactionState::GROWING);
    
    LockRequest req(txn->get_transaction_id(), LockMode::SHARED);
    req.granted_ = true;
    queue.request_queue_.push_back(req);

    // 更新 GroupLockMode (S > NON_LOCK)
    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
        queue.group_lock_mode_ = GroupLockMode::S;
    }
    
    return true;
}

/**
 * 行级 X 锁
 * 冲突：与任何锁（S 或 X）都冲突
 */
bool LockManager::lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_id];
    
    // Check if we already have a lock
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            if (it->lock_mode_ == LockMode::EXLUCSIVE) {
                return true; // Already have X lock
            }
            if (it->lock_mode_ == LockMode::SHARED) {
                // Upgrade S -> X
                // Check if there are other locks
                if (queue.request_queue_.size() > 1) {
                    txn->set_state(TransactionState::ABORTED);
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
                
                // No other locks, upgrade!
                it->lock_mode_ = LockMode::EXLUCSIVE;
                queue.group_lock_mode_ = GroupLockMode::X;
                return true;
            }
        }
    }

    // X 锁与任何现存锁冲突
    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    txn->get_lock_set()->insert(lock_id);
    txn->set_state(TransactionState::GROWING);

    // 注意：你的头文件拼写是 EXLUCSIVE
    LockRequest req(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    req.granted_ = true;
    queue.request_queue_.push_back(req);
    
    queue.group_lock_mode_ = GroupLockMode::X;
    return true;
}

/**
 * 表级 S 锁
 * 冲突：与 IX, X, SIX 冲突
 */
bool LockManager::lock_shared_on_table(Transaction *txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_id];
    // If txn already holds a table lock, ensure it is compatible with the requested mode,
    // or attempt an upgrade under no-wait.
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ != txn->get_transaction_id() || !req.granted_) {
            continue;
        }
        if (req.lock_mode_ == LockMode::SHARED || req.lock_mode_ == LockMode::EXLUCSIVE ||
            req.lock_mode_ == LockMode::S_IX) {
            return true;
        }
        if (req.lock_mode_ == LockMode::INTENTION_EXCLUSIVE) {
            // Upgrade IX -> SIX (S + IX) if no other granted locks exist.
            bool other_granted_exists = false;
            for (const auto &other : queue.request_queue_) {
                if (other.granted_ && other.txn_id_ != txn->get_transaction_id()) {
                    other_granted_exists = true;
                    break;
                }
            }
            if (other_granted_exists) {
                txn->set_state(TransactionState::ABORTED);
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            req.lock_mode_ = LockMode::S_IX;
            queue.group_lock_mode_ = GroupLockMode::SIX;
            return true;
        }
        if (req.lock_mode_ == LockMode::INTENTION_SHARED) {
            // Upgrade IS -> S if no conflicting locks.
            // (Under this project's no-wait policy, be conservative and require no other granted locks.)
            bool other_granted_exists = false;
            for (const auto &other : queue.request_queue_) {
                if (other.granted_ && other.txn_id_ != txn->get_transaction_id()) {
                    other_granted_exists = true;
                    break;
                }
            }
            if (other_granted_exists) {
                txn->set_state(TransactionState::ABORTED);
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            req.lock_mode_ = LockMode::SHARED;
            queue.group_lock_mode_ = GroupLockMode::S;
            return true;
        }
    }

    // 冲突检查
    if (queue.group_lock_mode_ == GroupLockMode::IX || 
        queue.group_lock_mode_ == GroupLockMode::X || 
        queue.group_lock_mode_ == GroupLockMode::SIX) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    txn->get_lock_set()->insert(lock_id);
    txn->set_state(TransactionState::GROWING);

    LockRequest req(txn->get_transaction_id(), LockMode::SHARED);
    req.granted_ = true;
    queue.request_queue_.push_back(req);

    // 更新逻辑: SIX > S > ...
    if (queue.group_lock_mode_ == GroupLockMode::IX) {
        queue.group_lock_mode_ = GroupLockMode::SIX;
    } else if (queue.group_lock_mode_ < GroupLockMode::S) {
        queue.group_lock_mode_ = GroupLockMode::S;
    }

    return true;
}

/**
 * 表级 X 锁
 * 冲突：与任何非空锁冲突
 */
bool LockManager::lock_exclusive_on_table(Transaction *txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_id];
    
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            if (it->lock_mode_ == LockMode::EXLUCSIVE) {
                return true;
            }
            // Upgrade to X
            if (queue.request_queue_.size() > 1) {
                 txn->set_state(TransactionState::ABORTED);
                 throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            it->lock_mode_ = LockMode::EXLUCSIVE;
            queue.group_lock_mode_ = GroupLockMode::X;
            return true;
        }
    }

    if (queue.group_lock_mode_ != GroupLockMode::NON_LOCK) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    txn->get_lock_set()->insert(lock_id);
    txn->set_state(TransactionState::GROWING);

    LockRequest req(txn->get_transaction_id(), LockMode::EXLUCSIVE);
    req.granted_ = true;
    queue.request_queue_.push_back(req);
    
    queue.group_lock_mode_ = GroupLockMode::X;
    return true;
}

/**
 * 表级 IS 锁
 * 冲突：只与 X 冲突
 */
bool LockManager::lock_IS_on_table(Transaction *txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_id];
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) return true;
    }

    if (queue.group_lock_mode_ == GroupLockMode::X) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    txn->get_lock_set()->insert(lock_id);
    txn->set_state(TransactionState::GROWING);

    LockRequest req(txn->get_transaction_id(), LockMode::INTENTION_SHARED);
    req.granted_ = true;
    queue.request_queue_.push_back(req);

    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
        queue.group_lock_mode_ = GroupLockMode::IS;
    }
    return true;
}

/**
 * 表级 IX 锁
 * 冲突：与 S, X, SIX 冲突
 */
bool LockManager::lock_IX_on_table(Transaction *txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_id];
    // If txn already holds a table lock, ensure it satisfies IX, or upgrade it.
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ != txn->get_transaction_id() || !req.granted_) {
            continue;
        }
        if (req.lock_mode_ == LockMode::EXLUCSIVE || req.lock_mode_ == LockMode::S_IX ||
            req.lock_mode_ == LockMode::INTENTION_EXCLUSIVE) {
            return true;
        }

        // Helper: check if any other transaction already holds a granted lock on this table.
        auto other_granted_exists = [&queue, txn]() {
            for (const auto &other : queue.request_queue_) {
                if (other.granted_ && other.txn_id_ != txn->get_transaction_id()) {
                    return true;
                }
            }
            return false;
        };

        if (req.lock_mode_ == LockMode::SHARED) {
            // Upgrade S -> SIX (S + IX). Must be exclusive with other holders under no-wait.
            if (other_granted_exists()) {
                txn->set_state(TransactionState::ABORTED);
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            req.lock_mode_ = LockMode::S_IX;
            queue.group_lock_mode_ = GroupLockMode::SIX;
            return true;
        }

        if (req.lock_mode_ == LockMode::INTENTION_SHARED) {
            // Upgrade IS -> IX is compatible with other IS holders.
            // Only conflict with existing S/X/SIX group modes.
            if (queue.group_lock_mode_ == GroupLockMode::S || queue.group_lock_mode_ == GroupLockMode::X ||
                queue.group_lock_mode_ == GroupLockMode::SIX) {
                txn->set_state(TransactionState::ABORTED);
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            req.lock_mode_ = LockMode::INTENTION_EXCLUSIVE;
            if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK || queue.group_lock_mode_ == GroupLockMode::IS) {
                queue.group_lock_mode_ = GroupLockMode::IX;
            }
            return true;
        }
    }

    if (queue.group_lock_mode_ == GroupLockMode::S ||
        queue.group_lock_mode_ == GroupLockMode::X || 
        queue.group_lock_mode_ == GroupLockMode::SIX) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    txn->get_lock_set()->insert(lock_id);
    txn->set_state(TransactionState::GROWING);

    LockRequest req(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);
    req.granted_ = true;
    queue.request_queue_.push_back(req);

    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK || queue.group_lock_mode_ == GroupLockMode::IS) {
        queue.group_lock_mode_ = GroupLockMode::IX;
    } else if (queue.group_lock_mode_ == GroupLockMode::S) {
        queue.group_lock_mode_ = GroupLockMode::SIX;
    }

    return true;
}

/**
 * 解锁
 */
bool LockManager::unlock(Transaction *txn, LockDataId lock_data_id) {
    std::unique_lock<std::mutex> lock(latch_);
    
    // 2PL: 解锁进入收缩阶段
    if (txn->get_state() == TransactionState::GROWING) {
        txn->set_state(TransactionState::SHRINKING);
    }

    auto &queue = lock_table_[lock_data_id];
    auto it = queue.request_queue_.begin();
    bool found = false;

    // 移除请求
    while (it != queue.request_queue_.end()) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            it = queue.request_queue_.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    
    if (found) {
        txn->get_lock_set()->erase(lock_data_id);
        
        // 重新计算 GroupLockMode
        queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
        for (const auto &req : queue.request_queue_) {
            if (req.granted_) {
                if (req.lock_mode_ == LockMode::EXLUCSIVE) {
                    queue.group_lock_mode_ = GroupLockMode::X; break;
                }
                if (req.lock_mode_ == LockMode::S_IX) {
                    queue.group_lock_mode_ = GroupLockMode::SIX; continue;
                }
                if (req.lock_mode_ == LockMode::SHARED) {
                    if (queue.group_lock_mode_ != GroupLockMode::SIX && queue.group_lock_mode_ != GroupLockMode::X)
                         queue.group_lock_mode_ = (queue.group_lock_mode_ == GroupLockMode::IX ? GroupLockMode::SIX : GroupLockMode::S);
                }
                if (req.lock_mode_ == LockMode::INTENTION_EXCLUSIVE) {
                     if (queue.group_lock_mode_ == GroupLockMode::S) queue.group_lock_mode_ = GroupLockMode::SIX;
                     else if (queue.group_lock_mode_ < GroupLockMode::IX) queue.group_lock_mode_ = GroupLockMode::IX;
                }
                if (req.lock_mode_ == LockMode::INTENTION_SHARED) {
                    if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) queue.group_lock_mode_ = GroupLockMode::IS;
                }
            }
        }
        return true;
    }
    return false;
}