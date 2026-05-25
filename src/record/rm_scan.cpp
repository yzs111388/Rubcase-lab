/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    file_handle_ = file_handle;  
    rid_ = {RM_FIRST_RECORD_PAGE, -1};
    next();
}
/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    
    if (is_end()) {
        return;  // 如果已经到达末尾，不再继续
    }
    
    int current_page = rid_.page_no;
    int current_slot = rid_.slot_no + 1;  // 从下一个槽位开始搜索
    
    // 获取文件头信息
    const RmFileHdr &file_hdr = file_handle_->file_hdr_;
    int num_pages = file_hdr.num_pages;
    
    while (current_page < num_pages) {
        // 获取当前页面的句柄
        RmPageHandle page_handle = file_handle_->fetch_page_handle(current_page);
        const RmPageHdr *page_hdr = page_handle.page_hdr;
        int num_slots = file_hdr.num_records_per_page;
        
        // 在当前页面中搜索有记录的槽位
        while (current_slot < num_slots) {
            if (Bitmap::is_set(page_handle.bitmap, current_slot)) {
                // 找到有记录的槽位
                rid_.page_no = current_page;
                rid_.slot_no = current_slot;
                return;
            }
            current_slot++;
        }
        
        // 当前页面搜索完毕，转到下一页
        current_page++;
        current_slot = 0;  // 从下一页的第一个槽位开始
    }
    
    // 没有找到更多记录，设置结束标志
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // 判断是否到达文件末尾
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}