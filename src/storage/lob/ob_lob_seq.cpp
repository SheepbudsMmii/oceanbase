/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX STORAGE

#include "ob_lob_seq.h"
namespace oceanbase
{
namespace storage
{

inline char *store32be(char *ptr, uint32_t val) {
  val = htonl(val);
  memcpy(ptr, &val, sizeof(val));
  return ptr + sizeof(val);
}

inline uint32_t load32be(const char *ptr) {
  uint32_t val;
  memcpy(&val, ptr, sizeof(val));
  return ntohl(val);
}

ObLobSeqId::ObLobSeqId(const ObString& seq_id, ObIAllocator* allocator)
  : allocator_(allocator),
    seq_id_(seq_id),
    read_only_(false),
    parsed_(false),
    last_seq_buf_pos_(0),
    buf_(NULL),
    len_(0),
    cap_(0),
    digits_(NULL),
    dig_len_(0),
    dig_cap_(0)
{
}

ObLobSeqId::ObLobSeqId(ObIAllocator* allocator)
  : ObLobSeqId()
{
  allocator_ = allocator;
}

ObLobSeqId::ObLobSeqId()
  : allocator_(NULL),
    seq_id_(),
    read_only_(false),
    parsed_(false),
    last_seq_buf_pos_(0),
    buf_(NULL),
    len_(0),
    cap_(0),
    digits_(NULL),
    dig_len_(0),
    dig_cap_(0)
{
}

void ObLobSeqId::reset()
{
  reset_digits();
  reset_seq_buf();
  seq_id_ = ObString();
  parsed_ = false;
}

void ObLobSeqId::reset_digits()
{
  if (digits_) {
    allocator_->free(digits_);
    digits_ = NULL;
  }
  dig_cap_ = 0;
  dig_len_ = 0;
}

void ObLobSeqId::reset_seq_buf()
{
  if (buf_) {
    allocator_->free(buf_);
    buf_ = NULL;
  }
  cap_ = 0;
  len_ = 0;
}

void ObLobSeqId::set_seq_id(ObString& seq_id)
{
  seq_id_ = seq_id;
}

void ObLobSeqId::set_allocator(ObIAllocator* allocator)
{
  allocator_ = allocator;
}

ObLobSeqId::~ObLobSeqId()
{
  if (allocator_) {
    if (buf_) {
      allocator_->free(buf_);
    }

    if (digits_) {
      allocator_->free(digits_);
    }
  }
}

int ObLobSeqId::get_next_seq_id(ObString& seq_id)
{
  int ret = OB_SUCCESS;
  if (!parsed_ &&
      OB_FAIL(parse())) {
    LOG_WARN("failed get next seq id, parse failed.", K(ret), K(seq_id_));
  } else if (empty()) {
    if (OB_FAIL(add_digits(LOB_SEQ_ID_MIN)) || OB_FAIL(append_seq_buf(LOB_SEQ_ID_MIN))) {
      LOG_WARN("failed add seq node.", K(ret));
    } else {
      seq_id.assign_ptr(buf_, len_);
    }
  } else {
    uint64_t cur = digits_[dig_len_ - 1];
    cur += LOB_SEQ_STEP_LEN;
    if (cur > UINT_MAX32) {
      if (OB_FAIL(add_digits(LOB_SEQ_ID_MIN)) || OB_FAIL(append_seq_buf(LOB_SEQ_ID_MIN))) {
        LOG_WARN("failed add seq node.", K(ret), K(len_), K(cap_), K(dig_len_), K(dig_cap_));
      } else {
        seq_id.assign_ptr(buf_, len_);
      }
    } else if (OB_FAIL(replace_seq_buf_last_node(static_cast<uint32_t>(cur)))) {
      LOG_WARN("failed replace seq node.", K(ret), K(len_), K(cap_), K(dig_len_), K(dig_cap_));
    } else {
      seq_id.assign_ptr(buf_, len_);
    }
  }
  return ret;
}

// just return origin seq id 
void ObLobSeqId::get_seq_id(ObString& seq_id)
{
  seq_id.assign_ptr(seq_id_.ptr(), seq_id_.length());
}

int ObLobSeqId::init_digits()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("failed init digits, allocator is null.", K(ret));
  } else if (OB_ISNULL(digits_ = static_cast<uint32_t*>(allocator_->alloc(LOB_SEQ_DIGIT_DEFAULT_LEN * sizeof(uint32_t))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed init digits, allocator digits buf null.", K(ret));
  } else {
    MEMSET(digits_, 0, LOB_SEQ_DIGIT_DEFAULT_LEN * sizeof(uint32_t));
    dig_len_ = 0;
    dig_cap_ = LOB_SEQ_DIGIT_DEFAULT_LEN;
  }
  return ret;
}

int ObLobSeqId::init_seq_buf()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("failed init seq buf, allocator is null.", K(ret));
  } else if (OB_ISNULL(buf_ = static_cast<char*>(allocator_->alloc(LOB_SEQ_DEFAULT_SEQ_BUF_LEN)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed init seq buf, allocator buf null.", K(ret));
  } else {
    MEMSET(buf_, 0, LOB_SEQ_DEFAULT_SEQ_BUF_LEN);
    len_ = 0;
    cap_ = LOB_SEQ_DEFAULT_SEQ_BUF_LEN;
  }
  return ret;
}

int ObLobSeqId::add_digits(uint32_t val)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(digits_) && OB_FAIL(init_digits())) {
    LOG_WARN("failed init digits, init digits failed.", K(ret));
  } else if (dig_len_ < dig_cap_) {
    digits_[dig_len_] = val;
    ++dig_len_;
  } else if (OB_FAIL(extend_digits())) {
    LOG_WARN("failed add digits, extend buf null.", K(ret));
  } else {
    ret = add_digits(val);
  }

  return ret;
}

int ObLobSeqId::append_seq_buf(uint32_t val)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf_) && OB_FAIL(init_seq_buf())) {
    LOG_WARN("failed add seq buf, init seq buf failed.", K(ret));
  } else if (len_ + sizeof(val) < cap_) {
    last_seq_buf_pos_ = len_;
    (void)store32be(buf_ + len_, val);
    len_ += sizeof(val);
  } else if (OB_FAIL(extend_seq_buf())) {
    LOG_WARN("failed extend seq buf, extend buf null.", K(ret), K(len_), K(cap_));
  } else {
    ret = append_seq_buf(val);
  }

  return ret;
}

int ObLobSeqId::replace_seq_buf_last_node(uint32_t val)
{
  int ret = OB_SUCCESS;
  if (last_seq_buf_pos_ + sizeof(val) < cap_) {
    if (dig_len_ == 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed replace seq digits array, digits is empty.", K(ret), K(dig_len_), K(dig_cap_));
    } else {
      digits_[dig_len_ - 1] = val;
      (void)store32be(buf_ + last_seq_buf_pos_, val);
      len_ = last_seq_buf_pos_ + sizeof(val);
    }
  } else if (OB_FAIL(extend_seq_buf())) {
    LOG_WARN("failed extend seq buf, extend buf null.", K(ret), K(len_), K(cap_));
  } else {
    ret = replace_seq_buf_last_node(val);
  }
  return ret;
}

int ObLobSeqId::extend_digits()
{
  int ret = OB_SUCCESS;
  uint32_t extend_len = dig_cap_ * 2 * sizeof(uint32_t);
  uint32_t* extend_dig = NULL;
  if (OB_ISNULL(extend_dig = static_cast<uint32_t*>(allocator_->alloc(extend_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed extend dig buf, allocator buf null.", K(ret));
  } else {
    MEMCPY(extend_dig, digits_, dig_cap_ * sizeof(uint32_t));
    MEMSET(reinterpret_cast<char*>(extend_dig) + dig_cap_ * sizeof(uint32_t),
           0, dig_cap_ * sizeof(uint32_t));
    dig_cap_ = dig_cap_ * 2;
    allocator_->free(digits_);
    digits_ = extend_dig;
  }
  return ret;
}

int ObLobSeqId::extend_seq_buf()
{
  int ret = OB_SUCCESS;
  uint32_t extend_len = cap_ * 2;
  char* extend_buf = NULL;
  if (OB_ISNULL(extend_buf = static_cast<char*>(allocator_->alloc(extend_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed init seq buf, allocator buf null.", K(ret));
  } else {
    MEMCPY(extend_buf, buf_, cap_);
    MEMSET(extend_buf + cap_, 0, cap_);
    cap_ = extend_len;
    allocator_->free(buf_);
    buf_ = extend_buf;
  }
  return ret;
}

int ObLobSeqId::empty()
{
  int ret = 0;
  if (dig_len_ == 0) {
    ret = 1;
  }
  return ret;
}

int ObLobSeqId::parse()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("failed parse seq_id, allocator is null.", K(ret));
  } else {
    ObString tmp_seq = seq_id_;
    size_t len = tmp_seq.length();

    if ((len % sizeof(uint32_t)) > 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail parse seq, internel unexpected.", K(ret), K(len));
    } else {
      const uint32_t* ori_dig = reinterpret_cast<const uint32_t*>(tmp_seq.ptr());
      const uint32_t ori_len = len / sizeof(uint32_t);
      uint32_t cur_pos = 0;
      while (OB_SUCC(ret) && cur_pos < ori_len) {
        uint32_t val = load32be(tmp_seq.ptr() + sizeof(uint32_t) * cur_pos);
        if (OB_FAIL(add_digits(val))) {
          LOG_WARN("add_digits failed.", K(ret), K(val));
        } else if (OB_FAIL(append_seq_buf(val))) {
          LOG_WARN("append_seq_buf failed.", K(ret), K(val));
        } else {
          cur_pos++;
        }
      } // end while
    }
  }

  if (OB_SUCC(ret)) {
    parsed_ = true;
  }

  return ret;
}

} // storage
} // oceanbase
