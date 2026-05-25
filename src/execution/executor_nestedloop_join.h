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
class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;   // 左儿子节点（外表）
    std::unique_ptr<AbstractExecutor> right_;  // 右儿子节点（内表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;                                 // 标记是否已到末尾

    /**
     * @brief 辅助函数：从左表或右表的元数据中查找特定列
     */
    const ColMeta& get_col_meta(const TabCol& tab_col) {
        // 检查左子节点
        for (const auto& col : left_->cols()) {
            if (col.tab_name == tab_col.tab_name && col.name == tab_col.col_name) {
                return col;
            }
        }
        // 检查右子节点
        for (const auto& col : right_->cols()) {
            if (col.tab_name == tab_col.tab_name && col.name == tab_col.col_name) {
                return col;
            }
        }
        throw ColumnNotFoundError(tab_col.tab_name + "." + tab_col.col_name);
    }

    /**
     * @brief 辅助函数：根据列名判断该列来自左表还是右表
     */
    const RmRecord* get_record_for_col(const TabCol& tab_col, const RmRecord* left_rec, const RmRecord* right_rec) {
        for (const auto& col : left_->cols()) {
            if (col.tab_name == tab_col.tab_name && col.name == tab_col.col_name) {
                return left_rec;
            }
        }
        // 如果不在左表，必定在右表
        return right_rec;
    }

    /**
     * @brief 评估单个连接条件
     */
    bool eval_cond(const RmRecord *left_rec, const RmRecord *right_rec, const Condition &cond) {
        // 获取条件左侧 (LHS) 的元数据和值
        const ColMeta& lhs_meta = get_col_meta(cond.lhs_col);
        const RmRecord* lhs_rec_for_cond = get_record_for_col(cond.lhs_col, left_rec, right_rec);
        const char *lhs_val = lhs_rec_for_cond->data + lhs_meta.offset;

        const char *rhs_val;
        ColType rhs_type;
        int rhs_len;

        if (cond.is_rhs_val) {
            // 条件右侧 (RHS) 是一个字面量
            assert(cond.rhs_val.raw != nullptr); // 必须在构造函数中被初始化
            rhs_val = cond.rhs_val.raw->data;
            rhs_type = cond.rhs_val.type;
            rhs_len = lhs_meta.len; // 字符串比较长度以LHS为准
        } else {
            // 条件右侧 (RHS) 是另一列
            const ColMeta& rhs_meta = get_col_meta(cond.rhs_col);
            const RmRecord* rhs_rec_for_cond = get_record_for_col(cond.rhs_col, left_rec, right_rec);
            rhs_val = rhs_rec_for_cond->data + rhs_meta.offset;
            rhs_type = rhs_meta.type;
            rhs_len = rhs_meta.len;
        }

        // 比较
        if (lhs_meta.type != rhs_type) {
            return false; // 类型不匹配
        }

        int cmp = ix_compare(lhs_val, rhs_val, lhs_meta.type, lhs_meta.len);
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

    /**
     * @brief 评估所有连接条件
     */
    bool eval_all_conds(const RmRecord *left_rec, const RmRecord *right_rec) {
        for (const auto &cond : fed_conds_) {
            if (!eval_cond(left_rec, right_rec, cond)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 核心辅助函数：查找下一个满足条件的元组对
     */
    void find_next_match() {
        while (true) {
            // 1. 检查左（外）表是否已结束
            if (left_->is_end()) {
                isend = true;
                return;
            }
            
            // 2. 检查右（内）表是否已结束
            if (right_->is_end()) {
                // 内表结束，推进外表
                left_->nextTuple();
                if (left_->is_end()) {
                    isend = true;
                    return;
                }
                // 重置内表
                right_->beginTuple();
                // 检查内表是否为空
                if (right_->is_end()) {
                    isend = true; // 如果内表为空，连接结果必定为空
                    return;
                }
            }

            // 3. 获取当前的外表和内表元组
            auto left_rec = left_->Next();
            auto right_rec = right_->Next();

            // 4. 检查是否满足连接条件
            if (eval_all_conds(left_rec.get(), right_rec.get())) {
                // 找到匹配项，停止循环。当前 (left_, right_) 指向的就是匹配的元组对
                return;
            }

            // 5. 不满足条件，推进内表，继续循环
            right_->nextTuple();
        }
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        
        // 合并元数据
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen(); // 调整右表列的偏移量
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        
        isend = false;
        fed_conds_ = std::move(conds);

        // 预处理
        // 修复在循环中调用 init_raw 的问题
        for (auto& cond : fed_conds_) {
            if (cond.is_rhs_val && cond.rhs_val.raw == nullptr) {
                // 值的类型和长度需要参照LHS列
                const ColMeta& lhs_meta = get_col_meta(cond.lhs_col);
                if (cond.rhs_val.type != lhs_meta.type) {
                     throw IncompatibleTypeError(coltype2str(lhs_meta.type), coltype2str(cond.rhs_val.type));
                }
                cond.rhs_val.init_raw(lhs_meta.len);
            }
        }
    }

    /**
     * @brief 定位到第一个满足连接条件的元组对
     */
    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        isend = false;

        // 查找第一个匹配项
        find_next_match();
    }

    /**
     * @brief 定位到下一个满足连接条件的元组对
     */
    void nextTuple() override {
        if (is_end()) {
            return;
        }
        // 刚返回一个匹配项，我们从内表的下一个元组开始搜索
        right_->nextTuple();
        // 查找下一个匹配项
        find_next_match();
    }

    /**
     * @brief 返回当前匹配的连接元组
     */
    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }

        // 获取当前的左右元组
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();

        // 创建新的连接元组
        auto joined_rec = std::make_unique<RmRecord>(len_);

        // 拷贝左元组数据
        memcpy(joined_rec->data, left_rec->data, left_->tupleLen());
        
        // 拷贝右元组数据（附加在左元组数据之后）
        memcpy(joined_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());

        return joined_rec;
    }

    // --- 其他必要的重写 ---

    bool is_end() const override { return isend; }

    Rid &rid() override { return _abstract_rid; } // Join 算子不产生 Rid

    size_t tupleLen() const override { return len_; }
    
    const std::vector<ColMeta> &cols() const override { return cols_; }
};
