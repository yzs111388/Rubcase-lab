/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/txn_defs.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

 std::unique_ptr<RmRecord> Next() override {
        // 遍历所有需要删除的记录
        for (const auto& rid : rids_) {

            // 0. 先获取删除所需的锁，避免在修改索引/数据后才因加锁失败而 abort
            if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
                context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
                context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
            }
            
            // 1. 获取即将被删除的记录
            //    (必须先获取，才能从中提取索引键)
            // 锁已在上面获取，这里避免重复加锁/升级导致的中途 abort
            std::unique_ptr<RmRecord> rec = fh_->get_record(rid, nullptr);
            if (rec == nullptr) {
                // 记录可能已被其他事务删除，跳过
                continue;
            }

            // 2. 写入回滚日志（必须在真正修改数据/索引之前）
            if (context_ != nullptr) {
                WriteRecord *write_record = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec);
                context_->txn_->append_write_record(write_record);
            }

            // 3. 从所有索引中删除该记录的条目
            //    (参照 InsertExecutor / UpdateExecutor)
            for (const auto& index_meta : tab_.indexes) {
                // a. 获取索引句柄
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta.cols)).get();

                // b. 从记录中构造索引键
                std::unique_ptr<char[]> key = std::make_unique<char[]>(index_meta.col_tot_len);
                int offset = 0;
                for(const auto& index_col : index_meta.cols) {
                    memcpy(key.get() + offset, rec->data + index_col.offset, index_col.len);
                    offset += index_col.len;
                }

                // c. 从索引中删除条目
                ih->delete_entry(key.get(), context_->txn_);
            }

            // 4. 在所有索引都维护好之后，最后删除记录文件中的记录
            //    锁已获取，这里避免重复加锁/升级
            fh_->delete_record(rid, nullptr);
        }

        // DML 语句执行完毕，返回 nullptr
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
