/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;

    std::vector<char> make_key(const RmRecord &rec, const IndexMeta &index) {
        std::vector<char> key(index.col_tot_len);
        int offset = 0;
        for (auto &col : index.cols) {
            memcpy(key.data() + offset, rec.data + col.offset, col.len);
            offset += col.len;
        }
        return key;
    }

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (context_ && context_->lock_mgr_ && context_->txn_) {
            context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        }
        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);
            if (context_ && context_->txn_) {
                context_->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec));
            }
            for (auto &index : tab_.indexes) {
                auto key = make_key(*rec, index);
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                ih->delete_entry(key.data(), context_ ? context_->txn_ : nullptr);
            }
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
