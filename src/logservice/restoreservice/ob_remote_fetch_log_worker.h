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

#ifndef OCEANBASE_LOGSERVICE_OB_REMOTE_FETCH_LOG_WORKER_H_
#define OCEANBASE_LOGSERVICE_OB_REMOTE_FETCH_LOG_WORKER_H_

#include "lib/queue/ob_lighty_queue.h"      // ObLightyQueue
#include "common/ob_queue_thread.h"         // ObCond
#include "share/ob_thread_pool.h"           // ObThreadPool
#include "share/ob_ls_id.h"                 // ObLSID

namespace oceanbase
{
namespace storage
{
class ObLSService;
}

namespace share
{
class SCN;
}

namespace palf
{
struct LSN;
class LogGroupEntry;
}

namespace logservice
{
class ObFetchLogTask;
class ObRemoteSourceGuard;
class ObRemoteLogParent;
class ObRemoteLogIterator;
class ObLogRestoreService;
using oceanbase::share::ObLSID;
using oceanbase::palf::LSN;
// Remote fetch log worker
class ObRemoteFetchWorker : public share::ObThreadPool
{
public:
  ObRemoteFetchWorker();
  ~ObRemoteFetchWorker();

  int init(const uint64_t tenant_id, ObLogRestoreService *restore_service, storage::ObLSService *ls_svr);
  void destroy();
  int start();
  void stop();
  void wait();
  void signal();
public:
  // submit fetch log task
  //
  // @retval OB_SIZE_OVERFLOW    task num more than queue limit
  // @retval other code          unexpected error
  int submit_fetch_log_task(ObFetchLogTask *task);

private:
  void run1();
  void do_thread_task_();
  int handle(ObFetchLogTask &task);
  int get_upper_limit_scn_(const ObLSID &id, share::SCN &scn);
  int submit_entries_(const ObLSID &id, const share::SCN &upper_limit_scn, ObRemoteLogIterator &iter, share::SCN &max_submit_scn);
  int cut_group_log_(const ObLSID &id, const LSN &lsn, const share::SCN &cut_scn, palf::LogGroupEntry &entry);
  int get_pre_accum_checksum_(const ObLSID &id, const LSN &lsn, int64_t &pre_accum_checksum);
  int submit_log_(const ObLSID &id, const LSN &lsn, char *buf, const int64_t buf_size);
  void mark_if_to_end_(ObFetchLogTask &task, const share::SCN &upper_limit_scn, const share::SCN &scn);
  int try_retire_(ObFetchLogTask *&task);
  void try_update_location_info_(const ObFetchLogTask &task, ObRemoteLogIterator &iter);

  bool is_retry_ret_code_(const int ret_code) const;
  bool is_fatal_error_(const int ret_code) const;
  void report_error_(const ObLSID &id, const int ret_code);
private:
  bool inited_;
  uint64_t tenant_id_;
  ObLogRestoreService *restore_service_;
  storage::ObLSService *ls_svr_;
  common::ObLightyQueue task_queue_;

  common::ObCond cond_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObRemoteFetchWorker);
};
} // namespace logservice
} // namespace oceanbase
#endif /* OCEANBASE_LOGSERVICE_OB_REMOTE_FETCH_LOG_WORKER_H_ */
