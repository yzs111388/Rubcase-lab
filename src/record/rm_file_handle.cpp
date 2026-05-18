/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "rm_file_handle.h"

std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    if (context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_shared_on_record(context->txn_, rid, fd_);
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return record;
}

Rid RmFileHandle::insert_record(char* buf, Context* context) {
    if (context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
    }
    RmPageHandle page_handle = create_page_handle();
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("RmFileHandle::insert_record no free slot");
    }
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    if (context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw InternalError("Invalid slot number");
    }
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records++;
    }
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        if (file_hdr_.first_free_page_no == rid.page_no) {
            file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        }
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    if (context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    bool was_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    if (was_full) {
        release_page_handle(page_handle);
    }
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    if (context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr) {
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no <= RM_FILE_HDR_PAGE || page_no >= file_hdr_.num_pages) {
        throw InternalError("Page does not exist");
    }
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        throw InternalError("RmFileHandle::fetch_page_handle failed");
    }
    return RmPageHandle(&file_hdr_, page);
}

RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) {
        throw InternalError("RmFileHandle::create_new_page_handle failed");
    }
    file_hdr_.num_pages++;
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    file_hdr_.first_free_page_no = page_id.page_no;
    return page_handle;
}

RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

void RmFileHandle::release_page_handle(RmPageHandle&page_handle) {
    page_id_t page_no = page_handle.page->get_page_id().page_no;
    if (file_hdr_.first_free_page_no == page_no) {
        return;
    }
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_no;
}
