/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <vector>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

namespace {
inline bool need_lock(Context* context) {
    return context != nullptr && context->lock_mgr_ != nullptr && context->txn_ != nullptr;
}

inline Transaction* txn_or_null(Context* context) {
    return context == nullptr ? nullptr : context->txn_;
}
}  // namespace

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    // 为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    // 创建系统目录
    DbMeta* new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description:
 * 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    // 数据库不存在
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }

    // 数据库已经打开
    if (!db_.name_.empty()) {
        throw DatabaseExistsError(db_name);
    }

    // 进入数据库目录
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }

    // 加载数据库元数据
    std::ifstream ifs(DB_META_NAME);
    if (!ifs) {
        throw UnixError();
    }
    ifs >> db_;  // 使用重载的>>操作符从文件读取数据库元数据

    // 打开所有表文件
    for (auto& entry : db_.tabs_) {
        auto& tab = entry.second;
        fhs_[tab.name] = rm_manager_->open_file(tab.name);

        // 打开该表的所有索引
        for (auto& index : tab.indexes) {
            std::string index_name = ix_manager_->get_index_name(tab.name, index.cols);
            ihs_[index_name] = ix_manager_->open_index(tab.name, index.cols);
        }
    }

    // 打开日志文件。返回 fd 不需要保存在 SmManager 中。
    disk_manager_->open_file(LOG_FILE_NAME);
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 检查数据库是否已经打开
    if (db_.name_.empty()) {
        throw DatabaseNotFoundError(db_.name_);
    }

    flush_meta();

    // 记录文件落盘并关闭
    for (auto& [_, file_handle] : fhs_) {
        if (file_handle != nullptr) {
            rm_manager_->close_file(file_handle.get());
        }
    }

    // 索引文件落盘并关闭
    for (auto& [_, index_handle] : ihs_) {
        if (index_handle != nullptr) {
            ix_manager_->close_index(index_handle.get());
        }
    }

    fhs_.clear();
    ihs_.clear();
    db_.name_.clear();
    db_.tabs_.clear();

    if (chdir("..") < 0) {  // 返回上一级目录
        throw UnixError();
    }
}

/**
 * @description:
 * 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto& entry : db_.tabs_) {
        auto& tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    // db_.get_table 会在表不存在时抛 TableNotFoundError，测试脚本会把它转为 failure。
    TabMeta& tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto& col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }

    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto& col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }

    int record_size = curr_offset;

    // Create & open record file
    rm_manager_->create_file(tab_name, record_size);
    fhs_[tab_name] = rm_manager_->open_file(tab_name);
    db_.tabs_[tab_name] = tab;

    if (need_lock(context) && fhs_.count(tab_name) && fhs_[tab_name] != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_[tab_name]->GetFd());
    }

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // 必须先判断表是否存在。不能先写 fhs_[tab_name]，否则不存在的表会被插入空句柄并导致崩溃。
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    if (need_lock(context) && fhs_.count(tab_name) && fhs_[tab_name] != nullptr) {
        context->lock_mgr_->lock_exclusive_on_table(context->txn_, fhs_[tab_name]->GetFd());
    }

    // 拷贝一份元数据，避免后续 erase 后引用失效。
    TabMeta tab = db_.get_table(tab_name);

    // 删除表的所有索引
    for (auto& index : tab.indexes) {
        std::string index_name = ix_manager_->get_index_name(tab_name, index.cols);

        // 关闭并移除索引句柄
        if (ihs_.count(index_name) > 0 && ihs_[index_name] != nullptr) {
            ix_manager_->close_index(ihs_[index_name].get());
            ihs_.erase(index_name);
        }

        // 删除索引文件。先判断文件存在，避免 destroy 不存在文件时抛异常导致服务端断开。
        if (disk_manager_->is_file(index_name)) {
            ix_manager_->destroy_index(tab_name, index.cols);
        }
    }

    // 关闭表文件，RmManager::close_file 内部会 flush 表文件脏页。
    if (fhs_.count(tab_name) > 0 && fhs_[tab_name] != nullptr) {
        rm_manager_->close_file(fhs_[tab_name].get());
        fhs_.erase(tab_name);
    }

    // 删除表文件
    if (disk_manager_->is_file(tab_name)) {
        rm_manager_->destroy_file(tab_name);
    }

    // 从数据库元数据中删除表并落盘
    db_.tabs_.erase(tab_name);
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 检查表是否存在。必须放在任何 fhs_[tab_name] 访问之前。
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    if (need_lock(context) && fhs_.count(tab_name) && fhs_[tab_name] != nullptr) {
        context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_[tab_name]->GetFd());
    }

    TabMeta& tab = db_.get_table(tab_name);

    // 检查索引是否已经存在
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    // 收集索引列的元数据
    std::vector<ColMeta> idx_cols;
    for (const auto& col_name : col_names) {
        auto it = std::find_if(tab.cols.begin(), tab.cols.end(), [&](const ColMeta& col) { return col.name == col_name; });
        if (it == tab.cols.end()) {
            throw ColumnNotFoundError(col_name);
        }
        idx_cols.push_back(*it);
    }

    // 计算索引列总长度
    int col_tot_len = 0;
    for (auto& col : idx_cols) {
        col_tot_len += col.len;
    }

    // 创建索引文件并打开索引文件
    ix_manager_->create_index(tab_name, idx_cols);
    std::string index_name = ix_manager_->get_index_name(tab_name, idx_cols);
    ihs_[index_name] = ix_manager_->open_index(tab_name, idx_cols);

    // 为表中的已有记录建立索引
    auto file_handle = fhs_[tab_name].get();
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        auto record = file_handle->get_record(scan.rid(), context);

        std::vector<char> key(col_tot_len);
        int offset = 0;
        for (auto& col : idx_cols) {
            memcpy(key.data() + offset, record->data + col.offset, col.len);
            offset += col.len;
        }

        ihs_[index_name]->insert_entry(key.data(), scan.rid(), txn_or_null(context));
    }

    // 创建索引元数据
    IndexMeta idx_meta;
    idx_meta.tab_name = tab_name;
    idx_meta.col_tot_len = col_tot_len;
    idx_meta.col_num = idx_cols.size();
    idx_meta.cols = idx_cols;
    tab.indexes.push_back(idx_meta);

    // 更新列的索引标志
    for (auto& col_name : col_names) {
        auto it = std::find_if(tab.cols.begin(), tab.cols.end(), [&](ColMeta& col) { return col.name == col_name; });
        if (it != tab.cols.end()) {
            it->index = true;
        }
    }

    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 检查表是否存在。必须放在任何 fhs_[tab_name] 访问之前。
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    if (need_lock(context) && fhs_.count(tab_name) && fhs_[tab_name] != nullptr) {
        context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_[tab_name]->GetFd());
    }

    TabMeta& tab = db_.get_table(tab_name);

    // 检查索引是否存在
    if (!tab.is_index(col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    auto idx_meta_iter = tab.get_index_meta(col_names);
    drop_index(tab_name, idx_meta_iter->cols, context);
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    // 检查表是否存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    if (need_lock(context) && fhs_.count(tab_name) && fhs_[tab_name] != nullptr) {
        context->lock_mgr_->lock_shared_on_table(context->txn_, fhs_[tab_name]->GetFd());
    }

    TabMeta& tab = db_.get_table(tab_name);

    std::string index_name = ix_manager_->get_index_name(tab_name, cols);

    // 关闭索引文件
    if (ihs_.count(index_name) > 0 && ihs_[index_name] != nullptr) {
        ix_manager_->close_index(ihs_[index_name].get());
        ihs_.erase(index_name);
    }

    // 删除索引文件
    if (disk_manager_->is_file(index_name)) {
        ix_manager_->destroy_index(tab_name, cols);
    }

    // 查找并删除索引元数据
    for (auto it = tab.indexes.begin(); it != tab.indexes.end(); ++it) {
        if (it->col_num == static_cast<int>(cols.size())) {
            bool match = true;
            for (size_t i = 0; i < cols.size(); i++) {
                if (it->cols[i].name != cols[i].name) {
                    match = false;
                    break;
                }
            }
            if (match) {
                tab.indexes.erase(it);
                break;
            }
        }
    }

    // 更新列的索引标志
    for (const auto& col : cols) {
        auto it = std::find_if(tab.cols.begin(), tab.cols.end(), [&](ColMeta& c) { return c.name == col.name; });
        if (it != tab.cols.end()) {
            it->index = false;
        }
    }

    flush_meta();
}
