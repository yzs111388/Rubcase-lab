/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

#include <cassert>
#include <iostream>

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    std::unique_lock<std::mutex> lock(latch_);
    
    // 1. 判断传入事务参数是否为空指针
    if (txn == nullptr) {
        // 2. 如果为空指针，创建新事务
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
        txn->set_state(TransactionState::GROWING);
    } else {
        // 开始已有事务
        txn->set_state(TransactionState::GROWING);
    }
    
    // 3. 把开始事务加入到全局事务表中
    txn_map[txn->get_transaction_id()] = txn;
    
    // 4. 返回当前事务指针
    return txn;
}

/**
 * @description: 事务的提交方法
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        std::cerr << "Error: Trying to commit null transaction" << std::endl;
        return;
    }
    
    txn_id_t txn_id = txn->get_transaction_id();
    
    // 1. 清理写操作记录
    auto write_set = txn->get_write_set();
    for (auto write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    
    // 2. 释放锁
    if (lock_manager_ != nullptr) {
        auto lock_set = txn->get_lock_set();
        // 复制一份锁集合，避免在遍历时修改集合导致迭代器失效
        std::vector<LockDataId> locks_to_unlock(lock_set->begin(), lock_set->end());
        for (const auto& lock_id : locks_to_unlock) {
            lock_manager_->unlock(txn, lock_id);
        }
        lock_set->clear();
    }
    
    // 3. 清理其他资源
    txn->get_index_latch_page_set()->clear();
    txn->get_index_deleted_page_set()->clear();
    
    // 4. 更新事务状态
    txn->set_state(TransactionState::COMMITTED);
    
    // 5. 从全局事务表中移除
    // 【重要修复】不要在这里移除事务，否则后续可能有组件调用 get_transaction 获取状态导致崩溃
    /*
    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn_id);
    }
    */
    
    std::cout << "Transaction " << txn_id << " committed successfully" << std::endl;
}

/**
 * @description: 事务的终止（回滚）方法
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        std::cerr << "Error: Trying to abort null transaction" << std::endl;
        return;
    }
    
    txn_id_t txn_id = txn->get_transaction_id();
    
    // 1. 回滚所有写操作
    auto write_set = txn->get_write_set();
    // 构建辅助 Context，用于调用 RM/IX 层接口
    Context ctx(lock_manager_, log_manager, txn);

    // 必须倒序回滚
    while (!write_set->empty()) {
        auto write_record = write_set->back();
        write_set->pop_back();

        // 获取记录信息
        auto &tab_name = write_record->GetTableName();
        auto &rid = write_record->GetRid();
        auto type = write_record->GetWriteType();

        // 获取表文件句柄和元数据
        auto file_handle = sm_manager_->fhs_.at(tab_name).get();
        auto &tab_meta = sm_manager_->db_.get_table(tab_name);

        if (type == WType::INSERT_TUPLE) {
            // 回滚插入 -> 执行删除
            // 1. 先读取当前记录（为了获取索引需要的key）
            auto record = file_handle->get_record(rid, &ctx);

            // 2. 从所有索引中删除
            for (auto &index : tab_meta.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                
                // 构建 Key
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(key + offset, record->data + col.offset, col.len);
                    offset += col.len;
                }
                
                // 从索引删除
                ih->delete_entry(key, txn);
                delete[] key;
            }

            // 3. 从数据文件中删除
            file_handle->delete_record(rid, &ctx);

        } else if (type == WType::DELETE_TUPLE) {
            // 回滚删除 -> 执行插入
            auto &record = write_record->GetRecord();

            // 1. 插入回数据文件 (指定RID插入)
            file_handle->insert_record(rid, record.data);

            // 2. 插入到所有索引
            for (auto &index : tab_meta.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(key + offset, record.data + col.offset, col.len);
                    offset += col.len;
                }
                
                ih->insert_entry(key, rid, txn);
                delete[] key;
            }

        } else if (type == WType::UPDATE_TUPLE) {
            // 回滚更新 -> 更新回旧值
            auto &old_record = write_record->GetRecord();
            // 获取当前新值（用于从索引删除新key）
            auto new_record = file_handle->get_record(rid, &ctx);

            // 1. 更新索引：删除新Key，插入旧Key
            for (auto &index : tab_meta.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();

                // 构建新Key（当前数据的Key）
                char *new_key = new char[index.col_tot_len];
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(new_key + offset, new_record->data + col.offset, col.len);
                    offset += col.len;
                }

                // 构建旧Key（WriteRecord中保存的旧数据Key）
                char *old_key = new char[index.col_tot_len];
                offset = 0;
                for (auto &col : index.cols) {
                    memcpy(old_key + offset, old_record.data + col.offset, col.len);
                    offset += col.len;
                }

                // 执行索引更新
                ih->delete_entry(new_key, txn);
                ih->insert_entry(old_key, rid, txn);

                delete[] new_key;
                delete[] old_key;
            }

            // 2. 更新数据文件
            file_handle->update_record(rid, old_record.data, &ctx);
        }

        delete write_record;
    }
    
    // 2. 释放锁
    if (lock_manager_ != nullptr) {
        auto lock_set = txn->get_lock_set();
        // 复制一份锁集合，避免在遍历时修改集合导致迭代器失效
        std::vector<LockDataId> locks_to_unlock(lock_set->begin(), lock_set->end());
        for (const auto& lock_id : locks_to_unlock) {
            lock_manager_->unlock(txn, lock_id);
        }
        lock_set->clear();
    }
    
    // 3. 清理其他资源
    txn->get_index_latch_page_set()->clear();
    txn->get_index_deleted_page_set()->clear();
    
    // 4. 更新事务状态
    txn->set_state(TransactionState::ABORTED);
    
    // 5. 从全局事务表中移除
    // 【重要修复】不要在这里移除事务，理由同 commit
    /*
    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn_id);
    }
    */
    
    std::cout << "Transaction " << txn_id << " aborted" << std::endl;
}