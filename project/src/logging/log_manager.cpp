/**
 * log_manager.cpp
 */

#include "logging/log_manager.h"

namespace cmudb {
/*
 * set ENABLE_LOGGING = true
 * Start a separate thread to execute flush to disk operation periodically
 * The flush can be triggered when the log buffer is full or buffer pool
 * manager wants to force flush (it only happens when the flushed page has a
 * larger LSN than persistent LSN)
 */
void LogManager::RunFlushThread() {
  if (!ENABLE_LOGGING) {
    ENABLE_LOGGING = true;

    std::unique_lock<std::mutex> lock(latch_);
    flush_thread_ = new std::thread([&]() {
      while (ENABLE_LOGGING) {
        // timeout?
        if (cv_.wait_for(lock, LOG_TIMEOUT) == std::cv_status::timeout) {
          std::swap(log_buffer_, flush_buffer_);
          offset_ = 0;
          flush_lsn_ = next_lsn_ - 1;
        }
        disk_manager_->WriteLog(flush_buffer_, LOG_BUFFER_SIZE);
        persistent_lsn_ = flush_lsn_;
      }
    });
  }
}

/*
 * Stop and join the flush thread, set ENABLE_LOGGING = false
 */
void LogManager::StopFlushThread() {
  if (ENABLE_LOGGING) {
    ENABLE_LOGGING = false;

    std::lock_guard<std::mutex> lock(latch_);
    cv_.notify_one();
    if (flush_thread_ && flush_thread_->joinable()) {
      flush_thread_->join();
    }
  }
}

/*
 * append a log record into log buffer
 * you MUST set the log record's lsn within this method
 * @return: lsn that is assigned to this log record
 *
 *
 * example below
 * // First, serialize the must have fields(20 bytes in total)
 * log_record.lsn_ = next_lsn_++;
 * memcpy(log_buffer_ + offset_, &log_record, 20);
 * int pos = offset_ + 20;
 *
 * if (log_record.log_record_type_ == LogRecordType::INSERT) {
 *    memcpy(log_buffer_ + pos, &log_record.insert_rid_, sizeof(RID));
 *    pos += sizeof(RID);
 *    // we have provided serialize function for tuple class
 *    log_record.insert_tuple_.SerializeTo(log_buffer_ + pos);
 *  }
 *
 */
lsn_t LogManager::AppendLogRecord(LogRecord &log_record) {
  std::lock_guard<std::mutex> lock(latch_);

  // log_buffer is almost full?
  if (offset_ + log_record.size_ > LOG_BUFFER_SIZE) {
    std::swap(log_buffer_, flush_buffer_);
    offset_ = 0;
    flush_lsn_ = next_lsn_ - 1;

    // wake up flush thread
    cv_.notify_one();
  }

  // First, serialize the must have fields(20 bytes in total)
  log_record.lsn_ = next_lsn_++;

  // for begin/commit/abort, we are done
  memcpy(log_buffer_ + offset_, &log_record, LogRecord::HEADER_SIZE);
  int pos = offset_ + LogRecord::HEADER_SIZE;

  if (log_record.log_record_type_ == LogRecordType::INSERT) {
    // for insert
    memcpy(log_buffer_ + pos, &log_record.insert_rid_, sizeof(RID));
    pos += sizeof(RID);
    log_record.insert_tuple_.SerializeTo(log_buffer_ + pos);

  } else if (log_record.log_record_type_ == LogRecordType::MARKDELETE ||
      log_record.log_record_type_ == LogRecordType::ROLLBACKDELETE ||
      log_record.log_record_type_ == LogRecordType::APPLYDELETE) {

    // for delete
    memcpy(log_buffer_ + pos, &log_record.delete_rid_, sizeof(RID));
    pos += sizeof(RID);
    log_record.delete_tuple_.SerializeTo(log_buffer_ + pos);

  } else if (log_record.log_record_type_ == LogRecordType::UPDATE) {
    // for update
    memcpy(log_buffer_ + pos, &log_record.update_rid_, sizeof(RID));
    pos += sizeof(RID);
    log_record.old_tuple_.SerializeTo(log_buffer_ + pos);
    pos += log_record.old_tuple_.GetLength();
    log_record.new_tuple_.SerializeTo(log_buffer_ + pos);

  } else if (log_record.log_record_type_ == LogRecordType::NEWPAGE) {
    // for new page
    memcpy(log_buffer_ + pos, &log_record.prev_page_id_, sizeof(page_id_t));
  }

  offset_ += log_record.size_;
  return log_record.lsn_;
}

} // namespace cmudb
