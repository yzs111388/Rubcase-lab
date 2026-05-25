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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    const ColMeta& get_col_meta(const std::string& col_name) const {
        for (const auto& col : cols_) {
            if (col.name == col_name) {
                return col;
            }
        }
        throw ColumnNotFoundError(col_name);
    }

    bool eval_cond(const RmRecord *rec, Condition &cond) {
        const ColMeta& lhs_meta = get_col_meta(cond.lhs_col.col_name);
        char *lhs_val = rec->data + lhs_meta.offset;
        char *rhs_val;
        if (cond.is_rhs_val) {
            if (cond.rhs_val.raw == nullptr) {
                cond.rhs_val.init_raw(lhs_meta.len);
            }
            rhs_val = cond.rhs_val.raw->data;
        } else {
            const ColMeta& rhs_meta = get_col_meta(cond.rhs_col.col_name);
            rhs_val = rec->data + rhs_meta.offset;
        }
        int cmp = ix_compare(lhs_val, rhs_val, lhs_meta.type, lhs_meta.len);
        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }

   void beginTuple() override {
    // 【任务三】添加表级 S 锁
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }

        // 【关键修复】必须初始化 scan_，否则会崩溃
        // 1. 获取索引名称
        std::string idx_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        
        // 2. 检查索引句柄是否存在
        if (sm_manager_->ihs_.find(idx_name) == sm_manager_->ihs_.end()) {
             throw IndexNotFoundError(tab_name_, index_col_names_);
        }
        
        // 3. 获取索引句柄
        auto* ih = sm_manager_->ihs_.at(idx_name).get();

        // 4. 初始化索引扫描迭代器 (IxScan)
        // 这里简化处理，传入 nullptr 表示扫描整个索引（或由 IxScan 内部处理条件），
        // 重点是传入 context_->txn_ 以便索引层也能进行并发控制
       Iid lower= ih->leaf_begin();
       Iid upper = ih->leaf_end();
       scan_= std::make_unique<IxScan>(ih, lower,upper, sm_manager_->get_bpm());

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (rec == nullptr) {
                scan_->next();
                continue;
            }
            bool match = true;
            for (auto &cond : fed_conds_) {
                if (!eval_cond(rec.get(), cond)) {
                    match = false;
                    break;
                }
            }
            if (match) break;
            scan_->next();
        }
    }
     
 

 void nextTuple() override {
    if (scan_ != nullptr && !scan_->is_end()) {
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (rec == nullptr) {
                scan_->next();
                continue;
            }
            bool match = true;
            for (auto &cond : fed_conds_) {
                if (!eval_cond(rec.get(), cond)) {
                    match = false;
                    break;
                }
            }
            if (match) break;
            scan_->next();
        }
    }
 }

 std::unique_ptr<RmRecord> Next() override {
    if (scan_ == nullptr || scan_->is_end()) {
            return nullptr;
    }
    rid_ = scan_->rid();
    return fh_->get_record(rid_, context_);
 }

 Rid &rid() override { return rid_; }
};