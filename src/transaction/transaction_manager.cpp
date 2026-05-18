/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    (void)log_manager;
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
        txn->set_txn_mode(false);
    }
    txn->set_start_ts(next_timestamp_++);
    txn->set_state(TransactionState::GROWING);
    std::scoped_lock lock{latch_};
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    (void)log_manager;
    if (txn == nullptr) return;
    std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
    for (auto &lock_id : locks) {
        lock_manager_->unlock(txn, lock_id);
    }
    for (auto *wr : *txn->get_write_set()) {
        delete wr;
    }
    txn->get_write_set()->clear();
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::COMMITTED);
}

static std::vector<char> make_index_key(const RmRecord &rec, const IndexMeta &index) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (auto &col : index.cols) {
        memcpy(key.data() + offset, rec.data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    (void)log_manager;
    if (txn == nullptr) return;

    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord *wr = *it;
        const std::string &tab_name = wr->GetTableName();
        auto &tab = sm_manager_->db_.get_table(tab_name);
        auto fh = sm_manager_->fhs_.at(tab_name).get();
        Rid rid = wr->GetRid();

        if (wr->GetWriteType() == WType::INSERT_TUPLE) {
            try {
                auto current = fh->get_record(rid, nullptr);
                for (auto &index : tab.indexes) {
                    auto key = make_index_key(*current, index);
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                    ih->delete_entry(key.data(), txn);
                }
                fh->delete_record(rid, nullptr);
            } catch (RMDBError &) {
                // The record may already have been removed by a partial rollback path.
            }
        } else if (wr->GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_rec = wr->GetRecord();
            fh->insert_record(rid, old_rec.data);
            for (auto &index : tab.indexes) {
                auto key = make_index_key(old_rec, index);
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                ih->insert_entry(key.data(), rid, txn);
            }
        } else if (wr->GetWriteType() == WType::UPDATE_TUPLE) {
            RmRecord &old_rec = wr->GetRecord();
            try {
                auto current = fh->get_record(rid, nullptr);
                for (auto &index : tab.indexes) {
                    auto key = make_index_key(*current, index);
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                    ih->delete_entry(key.data(), txn);
                }
            } catch (RMDBError &) {
                // Continue with restoration from the before image.
            }
            fh->update_record(rid, old_rec.data, nullptr);
            for (auto &index : tab.indexes) {
                auto key = make_index_key(old_rec, index);
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                ih->insert_entry(key.data(), rid, txn);
            }
        }
    }

    std::vector<LockDataId> locks(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
    for (auto &lock_id : locks) {
        lock_manager_->unlock(txn, lock_id);
    }
    for (auto *wr : *write_set) {
        delete wr;
    }
    write_set->clear();
    txn->get_lock_set()->clear();
    txn->set_state(TransactionState::ABORTED);
}
