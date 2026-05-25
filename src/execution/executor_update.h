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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses; // 复制
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;

    
        // 确保 SET 子句中的所有 Value 都被序列化 (init_raw)
        // 这样在 Next() 中就可以安全地重用它们
        for (auto& clause : set_clauses_) {
            // 查找被更新列的元数据
            auto col_it = std::find_if(tab_.cols.begin(), tab_.cols.end(), 
                                       [&](const ColMeta& col) { return col.name == clause.lhs.col_name; });
            
            if (col_it == tab_.cols.end()) {
                throw ColumnNotFoundError(clause.lhs.col_name);
            }
            const ColMeta& col_meta = *col_it;

            // 检查类型
            if (col_meta.type != clause.rhs.type) {
                throw IncompatibleTypeError(coltype2str(col_meta.type), coltype2str(clause.rhs.type));
            }
            
            // 序列化新值 - 仅在它尚未被初始化时
            if (clause.rhs.raw == nullptr) {
                clause.rhs.init_raw(col_meta.len); // 序列化 val.raw->data
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        // 遍历所有需要更新的记录
        for (const auto& rid : rids_) {
            // 1. 获取旧记录 (用于索引更新)
            std::unique_ptr<RmRecord> old_rec = fh_->get_record(rid, context_);
            if (old_rec == nullptr) {
                // 记录可能已被删除，跳过
                continue;
            }

            // 2. 创建新记录，并复制旧记录的数据
            size_t record_size = tab_.cols.back().offset + tab_.cols.back().len;
            RmRecord new_rec(record_size);
            memcpy(new_rec.data, old_rec->data, record_size);

            // 3. 将 set_clauses_ 应用到新记录上
            //    现在可以安全地使用 const&，因为 raw 数据已经在构造函数中准备好了
            for (const auto& clause : set_clauses_) { 
                // 查找被更新列的元数据
                auto col_it = std::find_if(tab_.cols.begin(), tab_.cols.end(), 
                                           [&](const ColMeta& col) { return col.name == clause.lhs.col_name; });
           
                if (col_it == tab_.cols.end()) {
                    throw ColumnNotFoundError(clause.lhs.col_name);
                }
                const ColMeta& col_meta = *col_it;
                
        
                // clause.rhs.init_raw(col_meta.len); // (已移至构造函数)

                // 确保 raw 数据存在 (在构造函数中已处理)
                assert(clause.rhs.raw != nullptr); 

                // 将新值写入新记录的缓冲区
                memcpy(new_rec.data + col_meta.offset, clause.rhs.raw->data, col_meta.len);
            }

            // 4. 更新索引 (参照 InsertExecutor)
            for (const auto& index_meta : tab_.indexes) {
                // 检查此索引的键是否被修改
                bool index_key_is_modified = false;
                for (const auto& index_col : index_meta.cols) {
                    for (const auto& clause : set_clauses_) {
                        if (index_col.name == clause.lhs.col_name) {
                            index_key_is_modified = true;
                            break;
                        }
                    }
                    if (index_key_is_modified) break;
                }

                // 如果索引键被修改了, 执行 "删除旧的, 插入新的"
                if (index_key_is_modified) {
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta.cols)).get();

                    // a. 构造旧键并删除
                    std::unique_ptr<char[]> old_key = std::make_unique<char[]>(index_meta.col_tot_len);
                    int offset = 0;
                    for(const auto& index_col : index_meta.cols) {
                        memcpy(old_key.get() + offset, old_rec->data + index_col.offset, index_col.len);
                        offset += index_col.len;
                    }
                    ih->delete_entry(old_key.get(), context_->txn_);

                    // b. 构造新键并插入
                    std::unique_ptr<char[]> new_key = std::make_unique<char[]>(index_meta.col_tot_len);
                    offset = 0;
                    for(const auto& index_col : index_meta.cols) {
                        memcpy(new_key.get() + offset, new_rec.data + index_col.offset, index_col.len);
                        offset += index_col.len;
                    }
                    ih->insert_entry(new_key.get(), rid, context_->txn_);
                }
            }

            // 5. 将更新后的记录写回磁盘
            fh_->update_record(rid, new_rec.data, context_);

            // Write Record for Rollback
            if (context_ != nullptr) {
                WriteRecord *write_record = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_rec);
                context_->txn_->append_write_record(write_record);
            }
        }

        // DML 语句执行完毕，返回 nullptr
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
