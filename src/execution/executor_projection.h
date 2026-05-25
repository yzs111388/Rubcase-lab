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

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;  // 投影节点的儿子节点
    std::vector<ColMeta> cols_;               // 需要投影的字段
    size_t len_;                              // 字段总长度
    std::vector<size_t> sel_idxs_;            // 选中列在子节点元组中的索引
   
   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);
        // context_ 继承自子节点
        context_ = prev_->context_; 

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    /**
     * @brief 定位到第一条结果元组。
     * 调用子节点的beginTuple()
     */
    void beginTuple() override {
        prev_->beginTuple();
    }

    /**
     * @brief 定位到下一条结果元组。
     * 调用子节点的nextTuple()
     */
    void nextTuple() override {
        prev_->nextTuple();
    }

    /**
     * @brief 返回投影后的元组
     * 1. 检查子节点是否结束
     * 2. 从子节点获取原始元组
     * 3. 创建新的投影元组
     * 4. 复制所需数据
     * 5. 返回新元组
     */
    std::unique_ptr<RmRecord> Next() override {
        // 1. 检查子节点是否已在末尾
        if (is_end()) {
            return nullptr;
        }

        // 2. 从子节点获取原始元组
        std::unique_ptr<RmRecord> src_rec = prev_->Next();
        if (src_rec == nullptr) {
            return nullptr;
        }

        // 3. 创建新的投影元组，大小为len_
        auto dst_rec = std::make_unique<RmRecord>(len_);

        // 4. 复制所需数据
        auto &prev_cols = prev_->cols(); // 获取子节点的列元数据
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            size_t src_idx = sel_idxs_[i]; // 源列在子元组中的索引
            
            const auto& src_col = prev_cols[src_idx]; // 源列的元数据 (用于获取offset)
            const auto& dst_col = cols_[i];         // 目标列的元数据 (用于获取offset和len)

            // 从源元组复制数据到目标元组
            memcpy(dst_rec->data + dst_col.offset, // 目标地址
                   src_rec->data + src_col.offset, // 源地址
                   dst_col.len);                 // 长度
        }

        // 5. 返回新元组
        return dst_rec;
    }

    /**
     * @brief 判断子节点是否没有结果了
     */
    bool is_end() const override {
        return prev_->is_end();
    }

    /**
     * @brief 返回投影后的列元数据
     */
    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    /**
     * @brief 返回投影后的元组长度
     */
    size_t tupleLen() const override {
        return len_;
    }

    Rid &rid() override { 
        // 投影元组没有自己的Rid，返回子节点的Rid
        return prev_->rid(); 
    }
};