/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;

    std::vector<Condition> fed_conds_;
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        context_ = left_->context_;
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        find_next_valid();
    }

    void nextTuple() override {
        if (isend) return;
        right_->nextTuple();
        find_next_valid();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend; }
    
    std::string getType() override { return "NestedLoopJoinExecutor"; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    private:
    void find_next_valid() {
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                auto left_rec = left_->Next();
                auto right_rec = right_->Next();
                if (eval_conds(left_rec.get(), right_rec.get(), fed_conds_, cols_)) {
                    isend = false;
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (left_->is_end()) break;
            right_->beginTuple();
        }
        isend = true;
    }

    std::unique_ptr<RmRecord> combine(const RmRecord *lhs_rec, const RmRecord *rhs_rec) const {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, lhs_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), rhs_rec->data, right_->tupleLen());
        return rec;
    }

    bool eval_cond(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const Condition &cond, const std::vector<ColMeta> &rec_cols) {
        auto combined = combine(lhs_rec, rhs_rec);
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        const char *lhs = combined->data + lhs_col->offset;
        const char *rhs = nullptr;
        Value rhs_val;
        if (cond.is_rhs_val) {
            rhs_val = cond.rhs_val;
            if (rhs_val.raw == nullptr) rhs_val.init_raw(lhs_col->len);
            rhs = rhs_val.raw->data;
        } else {
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs = combined->data + rhs_col->offset;
        }
        int cmp = ix_compare(lhs, rhs, lhs_col->type, lhs_col->len);
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

    bool eval_conds(const RmRecord *lhs_rec, const RmRecord *rhs_rec, const std::vector<Condition> &conds, const std::vector<ColMeta> &rec_cols) {
        return std::all_of(conds.begin(), conds.end(),
            [&](const Condition &cond) { return eval_cond(lhs_rec, rhs_rec, cond, rec_cols); }
        );
    }
};
