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
#include "record/rm.h"


class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;             // 表的名称
    std::vector<Condition> conds_;     // scan的条件
    RmFileHandle *fh_;                 // 表的数据文件句柄
    std::vector<ColMeta> cols_;        // scan后生成的记录的字段
    size_t len_;                       // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_; // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;  // table_iterator

    SmManager *sm_manager_;

    // Helper function to find ColMeta by name
    /**
     * @brief 按名称查找 ColMeta
     * @param col_name 列名
     * @return const ColMeta&
     * @throws ColumnNotFoundError
     */
    const ColMeta& get_col_meta(const std::string& col_name) const {
        for (const auto& col : cols_) {
            if (col.name == col_name) {
                return col;
            }
        }
        // This exception type might not exist, but it's good practice.
        // If it causes an error, replace with a standard exception.
        throw ColumnNotFoundError(col_name);
    }


    // Helper function to evaluate a single condition
    /**
     * @brief 评估单个条件
     * @param rec 记录
     * @param cond 条件
     * @return bool
     */
    bool eval_cond(const RmRecord *rec, Condition &cond) {
        // 1. 获取 lhs 的 ColMeta
        const ColMeta& lhs_meta = get_col_meta(cond.lhs_col.col_name);

        // 获取左侧值指针
        const char *lhs_val = rec->data + lhs_meta.offset;

        // 获取右侧值指针
        const char *rhs_val;

        if (cond.is_rhs_val) {
            // 右侧是一个值。确保 raw data 已初始化。
            // 仿照 InsertExecutor，我们需要调用 init_raw
            // Note: conds_ 必须是可修改的 (non-const) 才能调用 init_raw
            if (cond.rhs_val.raw == nullptr) {
                // 使用 lhs_meta.len 因为它们应该是相同类型
                cond.rhs_val.init_raw(lhs_meta.len);
            }
            rhs_val = cond.rhs_val.raw->data;
        } else {
            // 2. 获取 rhs 的 ColMeta
            const ColMeta& rhs_meta = get_col_meta(cond.rhs_col.col_name);
            // 右侧是另一列
            rhs_val = rec->data + rhs_meta.offset;
        }

        // 使用ix_compare执行比较
        // 3. 使用 lhs_meta 的 type 和 len
        int cmp_res = ix_compare(lhs_val, rhs_val, lhs_meta.type, lhs_meta.len);

        switch (cond.op) {
            case OP_EQ: return cmp_res == 0;
            case OP_NE: return cmp_res != 0;
            case OP_LT: return cmp_res < 0;
            case OP_GT: return cmp_res > 0;
            case OP_LE: return cmp_res <= 0;
            case OP_GE: return cmp_res >= 0;
            // 4. 移除了未定义的 OP_NO
            default: return false; // Unknown operation
        }
    }

    // Helper function to evaluate all conditions
    /**
     * @brief 评估所有条件
     * @param rec 记录
     * @return bool
     */
    bool eval_all_conds(const RmRecord *rec) {
        for (auto &cond : conds_) { // Note: non-const 引用以允许 init_raw
            if (!eval_cond(rec, cond)) {
                return false; // 任一条件失败
            }
        }
        return true; // 所有条件通过
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        // 修复拼写错误：cols_back() -> cols_.back()
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_; // Note: This field seems redundant with conds_
        
        // Initialize rid_ to an invalid state (end)
        rid_ = {.page_no = RM_NO_PAGE, .slot_no = -1};
    }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void beginTuple() override {
        if (context_ != nullptr) {
        context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
    }
        // 1. 创建表迭代器
        // 错误：不能实例化 RecScan（抽象类），应实例化 RmScan（具体类）
        scan_ = std::make_unique<RmScan>(fh_);

        // 2. 循环查找第一个匹配的记录
        while (!scan_->is_end()) {
            // 3. 获取当前记录
            std::unique_ptr<RmRecord> rec = fh_->get_record(scan_->rid(), context_);

            // 修复：添加空指针检查
            if (rec == nullptr) {
                // 记录可能已被删除或无效，跳过
                scan_->next();
                continue;
            }

            // 4. 检查是否满足所有条件
            if (eval_all_conds(rec.get())) {
                // 5. 如果满足, 设置 rid_ 并返回
                rid_ = scan_->rid();
                return;
            }

            // 6. 如果不满足, 移动到下一条物理记录
            scan_->next();
        }

        // 7. 如果没有找到记录, scan_ 会到达末尾.
        // 将 rid_ 设置为末尾迭代器的 rid (page_no 应为 RM_NO_PAGE)
        if (scan_->is_end()) {
            rid_ = scan_->rid();
        }
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到下一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void nextTuple() override {
        // 如果迭代器未初始化或已在末尾，则无法继续
        if (scan_ == nullptr || scan_->is_end()) {
            if(scan_) {
                rid_ = scan_->rid(); // 确保 rid_ 处于末尾状态
            }
            return;
        }

        // 1. 移动到下一条物理记录
        scan_->next();

        // 2. 循环查找下一条匹配的记录
        while (!scan_->is_end()) {
            // 3. 获取当前记录
            std::unique_ptr<RmRecord> rec = fh_->get_record(scan_->rid(), context_);

            // 修复：添加空指针检查
            if (rec == nullptr) {
                // 记录可能已被删除或无效，跳过
                scan_->next();
                continue;
            }

            // 4. 检查条件
            if (eval_all_conds(rec.get())) {
                // 5. 如果满足, 设置 rid_ 并返回
                rid_ = scan_->rid();
                return;
            }

            // 6. 如果不满足, 移动到下一条物理记录
            scan_->next();
        }

        // 7. 如果没有更多记录, scan_ 到达末尾. 更新 rid_
        if (scan_->is_end()) {
            rid_ = scan_->rid();
        }
    }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    std::unique_ptr<RmRecord> Next() override {
        // 如果 rid_ 无效 (即已在末尾), 返回 nullptr
        if (is_end()) {
            return nullptr;
        }
        // 获取并返回 rid_ 当前指向的记录
        return fh_->get_record(rid_, context_);
    }

    /**
     * @brief 检查扫描是否已到达末尾
     */
    bool is_end() const override {
        // RmScan::is_end() 检查 rid_.page_no == RM_NO_PAGE
        return rid_.page_no == RM_NO_PAGE;
    }

    /**
     * @brief 返回 executor 对应的 Rid
     */
    Rid &rid() override { return rid_; }

    /**
     * @brief 返回 executor 生成的元组的字段
     */
    const std::vector<ColMeta> &cols() const override { return cols_; }

    /**
     * @brief 返回 executor 生成的元组的长度
     */
    size_t tupleLen() const override { return len_; }
};

