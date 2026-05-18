/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "rm_scan.h"
#include "rm_file_handle.h"

RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_ = Rid{RM_FIRST_RECORD_PAGE, -1};
    next();
}

void RmScan::next() {
    int page_no = rid_.page_no;
    int slot_no = rid_.slot_no;
    while (page_no < file_handle_->file_hdr_.num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        int next_slot = Bitmap::next_bit(true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page, slot_no);
        file_handle_->buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        if (next_slot < file_handle_->file_hdr_.num_records_per_page) {
            rid_ = Rid{page_no, next_slot};
            return;
        }
        page_no++;
        slot_no = 0;
    }
    rid_ = Rid{file_handle_->file_hdr_.num_pages, 0};
}

bool RmScan::is_end() const {
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

Rid RmScan::rid() const {
    return rid_;
}
