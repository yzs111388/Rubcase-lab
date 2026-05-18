/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.empty() ? 0 : cols_.back().offset + cols_.back().len;
        context_ = context;
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        if (context_ && context_->lock_mgr_ && context_->txn_) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        scan_ = std::make_unique<RmScan>(fh_);
        while (!scan_->is_end()) {
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(rec.get(), fed_conds_, cols_)) {
                rid_ = scan_->rid();
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) return;
        scan_->next();
        while (!scan_->is_end()) {
            auto rec = fh_->get_record(scan_->rid(), context_);
            if (eval_conds(rec.get(), fed_conds_, cols_)) {
                rid_ = scan_->rid();
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }
    
    std::string getType() override { return "SeqScanExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    private:
    int cmp_value(const char *lhs, const char *rhs, ColType type, int len) const {
        return ix_compare(lhs, rhs, type, len);
    }

    bool eval_cond(const RmRecord *rec, const Condition &cond, const std::vector<ColMeta> &rec_cols) {
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        const char *lhs = rec->data + lhs_col->offset;
        const char *rhs = nullptr;
        Value rhs_val;
        if (cond.is_rhs_val) {
            rhs_val = cond.rhs_val;
            if (rhs_val.raw == nullptr) rhs_val.init_raw(lhs_col->len);
            rhs = rhs_val.raw->data;
        } else {
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs = rec->data + rhs_col->offset;
        }
        int cmp = cmp_value(lhs, rhs, lhs_col->type, lhs_col->len);
        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    }

    bool eval_conds(const RmRecord *rec, const std::vector<Condition> &conds, const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(),
            [&](const Condition &cond) { return eval_cond(rec, cond, rec_cols); }
        );
    }
};
