/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

namespace {
inline bool need_lock(Context *context) {
    return context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr;
}
}  // namespace

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    if (need_lock(context)) {
        context->lock_mgr_->lock_IS_on_table(context->txn_, fd_);
        context->lock_mgr_->lock_shared_on_record(context->txn_, rid, fd_);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    auto rm_record = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return rm_record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context) {
    if (need_lock(context)) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
    }

    RmPageHandle page_handle = create_page_handle();

    int slot_pos = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_pos == -1) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw std::runtime_error("No free slot found");
    }

    Rid rid{page_handle.page->get_page_id().page_no, slot_pos};
    if (need_lock(context)) {
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }

    memcpy(page_handle.get_slot(slot_pos), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_pos);
    page_handle.page_hdr->num_records++;

    // 如果插入后该页已满，则把它从空闲页链表中摘掉
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char*>(&file_hdr_), sizeof(file_hdr_));
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw std::runtime_error("Slot already occupied");
    }

    bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);

    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;

    // 一般不会从 full 页插入；这里保守处理插入后变满的情况
    if (!was_full && page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        if (file_hdr_.first_free_page_no == rid.page_no) {
            file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        }
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char*>(&file_hdr_), sizeof(file_hdr_));
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    if (need_lock(context)) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);

    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;

    // 注意：release_page_handle 必须在 unpin 前调用，否则会修改已经释放的 page handle
    if (was_full) {
        release_page_handle(page_handle);
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的位置
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    if (need_lock(context)) {
        context->lock_mgr_->lock_IX_on_table(context->txn_, fd_);
        context->lock_mgr_->lock_exclusive_on_record(context->txn_, rid, fd_);
    }

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < RM_FILE_HDR_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError("", page_no);
    }

    PageId page_id{fd_, page_no};
    Page *target_page = buffer_pool_manager_->fetch_page(page_id);
    if (target_page == nullptr) {
        throw PageNotExistError("", page_no);
    }

    return RmPageHandle(&file_hdr_, target_page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page *new_page = buffer_pool_manager_->new_page(&page_id);
    if (new_page == nullptr) {
        throw std::runtime_error("No buffer frame available when creating new record page");
    }

    file_hdr_.num_pages++;
    file_hdr_.first_free_page_no = page_id.page_no;

    RmPageHandle page_handle(&file_hdr_, new_page);
    page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    page_handle.page_hdr->num_records = 0;
    memset(page_handle.bitmap, 0, file_hdr_.bitmap_size);

    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char*>(&file_hdr_), sizeof(file_hdr_));
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

/**
 * @description: 当前 page 从满变为未满时，把它重新加入空闲页链表
 */
void RmFileHandle::release_page_handle(RmPageHandle &page_handle) {
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, reinterpret_cast<char*>(&file_hdr_), sizeof(file_hdr_));
}
