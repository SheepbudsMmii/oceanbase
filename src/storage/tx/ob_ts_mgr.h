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

#ifndef OCEANBASE_TRANSACTION_OB_TS_MGR_
#define OCEANBASE_TRANSACTION_OB_TS_MGR_

#include <stdint.h>
#include "lib/utility/ob_print_utils.h"
#include "lib/lock/ob_drw_lock.h"
#include "lib/hash/ob_link_hashmap.h"
#include "lib/atomic/atomic128.h"
#include "lib/net/ob_addr.h"
#include "lib/queue/ob_link_queue.h"
#include "lib/container/ob_iarray.h"
#include "share/ob_errno.h"
#include "share/ob_thread_pool.h"
#include "share/lock/ob_qsync_lock.h"
#include "ob_gts_source.h"
#include "ob_gts_define.h"
#include "ob_ts_worker.h"
#include "ob_location_adapter.h"

#define REFRESH_GTS_INTERVEL_US  (100 * 1000)

namespace oceanbase
{
namespace share
{
class ObLocationService;
namespace schema
{
class ObSchemaGetterGuard;
class ObMultiVersionSchemaService;
}
}
namespace obrpc
{
class ObGtsRpcProxy;
}
namespace rpc
{
namespace frame
{
class ObReqTransport;
}
}
namespace share
{
class SCN;
}
namespace transaction
{
class ObLocationAdapter;
class ObITsSource;
class ObGtsRequestRpc;
class ObIGlobalTimestampService;

class ObTsCbTask : public common::ObLink
{
public:
  ObTsCbTask() {}
  virtual ~ObTsCbTask() {}
  virtual int gts_callback_interrupted(const int errcode) = 0;
  virtual int get_gts_callback(const MonotonicTs srr, const share::SCN &gts, const MonotonicTs receive_gts_ts) = 0;
  virtual int gts_elapse_callback(const MonotonicTs srr, const share::SCN &gts) = 0;
  virtual MonotonicTs get_stc() const = 0;
  virtual uint64_t hash() const = 0;
  virtual uint64_t get_tenant_id() const = 0;
  VIRTUAL_TO_STRING_KV("", "");
};

class ObITsMgr
{
public:
  virtual int update_gts(const uint64_t tenant_id, const int64_t gts, bool &update) = 0;
  /*
  virtual int update_local_trans_version(const uint64_t tenant_id, const int64_t version, bool &update) = 0;
  */
  virtual int get_gts(const uint64_t tenant_id,
                      const MonotonicTs stc,
                      ObTsCbTask *task,
                      share::SCN &scn,
                      MonotonicTs &receive_gts_ts) = 0;
  virtual int get_gts(const uint64_t tenant_id, ObTsCbTask *task, share::SCN &scn) = 0;
  virtual int get_ts_sync(const uint64_t tenant_id, const int64_t timeout_ts,
      share::SCN &scn, bool &is_external_consistent) = 0;
  /*
  virtual int get_local_trans_version(const uint64_t tenant_id,
                                      const MonotonicTs stc,
                                      ObTsCbTask *task,
                                      int64_t &gts,
                                      MonotonicTs &receive_gts_ts) = 0;
  virtual int get_local_trans_version(const uint64_t tenant_id,
                                      ObTsCbTask *task,
                                      int64_t &gts) = 0;
                                      */
  virtual int wait_gts_elapse(const uint64_t tenant_id, const share::SCN &scn, ObTsCbTask *task,
                              bool &need_wait) = 0;
  virtual int wait_gts_elapse(const uint64_t tenant_id, const share::SCN &scn) = 0;
  virtual int update_base_ts(const int64_t base_ts) = 0;
  virtual int get_base_ts(int64_t &base_ts) = 0;
  virtual bool is_external_consistent(const uint64_t tenant_id) = 0;
public:
  VIRTUAL_TO_STRING_KV("", "");
};

class ObTsSourceInfo;
class ObTsSourceGuard
{
public:
  ObTsSourceGuard() : ts_source_(NULL), ts_source_info_(NULL), ts_type_(0) {}
  ~ObTsSourceGuard();
  void set(ObTsSourceInfo *info, ObITsSource *ts_source, const int ts_type)
  {
    ts_source_info_ = info;
    ts_source_ = ts_source;
    ts_type_ = ts_type;
  }
  void set_ts_source(ObITsSource *ts_source) { ts_source_ = ts_source; }
  void set_ts_source_info(ObTsSourceInfo *ts_source_info) { ts_source_info_ = ts_source_info; }
  ObITsSource *get_ts_source() { return ts_source_; }
  void set_ts_type(int ts_type) { ts_type_ = ts_type; }
  int get_ts_type() { return ts_type_; }
private:
  ObITsSource *ts_source_;
  ObTsSourceInfo *ts_source_info_;
  int ts_type_;
};

typedef common::LinkHashNode<ObTsTenantInfo> ObTsTenantInfoNode;
typedef common::LinkHashValue<ObTsTenantInfo> ObTsTenantInfoValue;
class ObTsSourceInfo : public ObTsTenantInfoValue
{
  friend class ObTsSourceGuard;
public:
  ObTsSourceInfo();
  ~ObTsSourceInfo() { destroy(); }
  int init(const uint64_t tenant_id);
  void destroy();
public:
  uint64_t get_tenant_id() const { return tenant_id_; }
  ObGtsSource *get_gts_source() { return &gts_source_; }
  int get_ts_source(const uint64_t tenant_id, ObTsSourceGuard &guard, bool &is_valid);
  int check_and_switch_ts_source(const uint64_t tenant_id);
  void update_last_access_ts() { last_access_ts_ = common::ObClockGenerator::getClock(); }
  int64_t get_last_access_ts() const { return last_access_ts_; }
  int64_t get_last_obtain_switch_ts() const { return last_obtain_switch_ts_; }
  int set_invalid();
  int check_if_tenant_has_been_dropped(const uint64_t tenant_id, bool &has_dropped);
  int gts_callback_interrupted(const int errcode);
  int switch_ts_source(const uint64_t tenant_id, const int ts_type);
private:
  int switch_ts_source_(const uint64_t tenant_id, const int ts_type);
  void revert_ts_source_(ObTsSourceGuard &guard);
private:
  static const int64_t DEFAULT_CHECK_SWITCH_INTERVAL_US = 100 * 1000;
  static const int64_t MAX_CHECK_SWITCH_INTERVAL_US = 3 * 1000 * 1000;
private:
  bool is_inited_;
  bool is_valid_;
  uint64_t tenant_id_;
  int64_t last_check_switch_ts_;
  int64_t last_obtain_switch_ts_;
  int64_t check_switch_interval_;
  ObITsSource *ts_source_[MAX_TS_SOURCE];
  int cur_ts_type_;
  ObGtsSource gts_source_;
  mutable share::ObQSyncLock rwlock_;
  int64_t last_access_ts_ CACHE_ALIGNED;
};

class ObTsSourceInfoAlloc
{
public:
  static ObTsSourceInfo *alloc_value() { return NULL; }
  static void free_value(ObTsSourceInfo *info)
  {
    if (NULL != info) {
      info->~ObTsSourceInfo();
      ob_free(info);
      info = NULL;
    }
  }
  static ObTsTenantInfoNode *alloc_node(ObTsSourceInfo *p)
  {
    UNUSED(p);
    return op_alloc(ObTsTenantInfoNode);
  }
  static void free_node(ObTsTenantInfoNode *node)
  {
    if (NULL != node) {
      op_free(node);
      node = NULL;
    }
  }
};

class ObGtsRefreshFunctor
{
public:
  ObGtsRefreshFunctor() {}
  ~ObGtsRefreshFunctor() {}
  bool operator()(const ObTsTenantInfo &gts_tenant_info, ObTsSourceInfo *ts_source_info)
  {
    int ret = common::OB_SUCCESS;
    ObGtsSource *gts_source = NULL;
    if (OB_ISNULL(ts_source_info)) {
      ret = common::OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "ts source info is null", KR(ret));
    } else if (OB_FAIL(ts_source_info->check_and_switch_ts_source(gts_tenant_info.get_value()))) {
      TRANS_LOG(WARN, "check and switch ts source failed", KR(ret), K(gts_tenant_info));
    } else if (NULL == (gts_source = (ts_source_info->get_gts_source()))) {
      ret = common::OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "gts cache queue is null", KR(ret), K(gts_tenant_info));
    } else {
      if (OB_FAIL(gts_source->refresh_gts(false))) {
        if (EXECUTE_COUNT_PER_SEC(1)) {
          TRANS_LOG(WARN, "refresh gts failed", KR(ret), K(gts_tenant_info));
        }
      }
      if (EXECUTE_COUNT_PER_SEC(1)) {
        TRANS_LOG(INFO, "refresh gts functor", KR(ret), K(gts_tenant_info));
      }
      // rewrite ret
      ret = common::OB_SUCCESS;
    }
    return true;
  }
};

class GetObsoleteTenantFunctor
{
public:
  GetObsoleteTenantFunctor(const int64_t obsolete_time, common::ObIArray<uint64_t> &array)
      : obsolete_time_(obsolete_time), array_(array)
  {
    array_.reset();
  }
  ~GetObsoleteTenantFunctor() {}
  bool operator()(const ObTsTenantInfo &gts_tenant_info, ObTsSourceInfo *ts_source_info)
  {
    int ret = common::OB_SUCCESS;
    const int64_t now = common::ObClockGenerator::getClock();
    if (OB_ISNULL(ts_source_info)) {
      ret = common::OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "ts source info is null", KR(ret));
    } else if (now - ts_source_info->get_last_access_ts() < obsolete_time_) {
      // do nothing
    } else if (now - ts_source_info->get_last_obtain_switch_ts() < obsolete_time_) {
      // do nothing
    } else if (OB_FAIL(array_.push_back(gts_tenant_info.get_value()))) {
      TRANS_LOG(WARN, "push back tenant failed", K(ret), K(gts_tenant_info));
    } else {
      // do nothing
    }
    //外面需要遍历所有的租户，此处不能返回false
    return true;
  }
private:
  const int64_t obsolete_time_;
  common::ObIArray<uint64_t> &array_;
};

class CheckTenantFunctor
{
public:
  CheckTenantFunctor(common::ObIArray<uint64_t> &array)
      : array_(array)
  {
    array_.reset();
  }
  ~CheckTenantFunctor() {}
  bool operator()(const ObTsTenantInfo &gts_tenant_info, ObTsSourceInfo *ts_source_info)
  {
    int ret = common::OB_SUCCESS;
    ObGtsSource *gts_source = NULL;
    bool has_dropped = false;
    if (OB_ISNULL(ts_source_info)) {
      ret = common::OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "ts source info is null", KR(ret));
    } else if (OB_FAIL(ts_source_info->check_if_tenant_has_been_dropped(gts_tenant_info.get_value(), has_dropped))) {
      TRANS_LOG(WARN, "check and switch ts source failed", KR(ret), K(gts_tenant_info));
    } else if (has_dropped) {
      if (OB_FAIL(array_.push_back(gts_tenant_info.get_value()))) {
        TRANS_LOG(WARN, "push back tenant failed", K(ret), K(gts_tenant_info));
      }
    }
    return true;
  }
private:
  common::ObIArray<uint64_t> &array_;
};

class ObTsMgr;
class ObTsSourceInfoGuard
{
public:
  ObTsSourceInfoGuard() : ts_source_info_(NULL), mgr_(NULL), need_revert_(true) {}
  ~ObTsSourceInfoGuard();
  void set(ObTsSourceInfo *info, ObTsMgr *mgr, const bool need_revert)
  {
    ts_source_info_ = info;
    mgr_ = mgr;
    need_revert_ = need_revert;
  }
  void set_ts_source_info(ObTsSourceInfo *ts_source_info) { ts_source_info_ = ts_source_info; }
  void set_mgr(ObTsMgr *mgr) { mgr_ = mgr; }
  void set_need_revert(const bool need_revert) { need_revert_ = need_revert; }
  ObTsSourceInfo *get_ts_source_info() { return ts_source_info_; }
  bool need_revert() const { return need_revert_; }
private:
  ObTsSourceInfo *ts_source_info_;
  ObTsMgr *mgr_;
  bool need_revert_;
};

typedef common::ObLinkHashMap<ObTsTenantInfo, ObTsSourceInfo, ObTsSourceInfoAlloc> ObTsSourceInfoMap;
class ObTsMgr : public share::ObThreadPool, public ObITsMgr
{
  friend class ObTsSourceInfoGuard;
public:
  ObTsMgr() { reset(); }
  ~ObTsMgr() { destroy(); }
  int init(const common::ObAddr &server,
           share::schema::ObMultiVersionSchemaService &schema_service,
           share::ObLocationService &location_service,
           rpc::frame::ObReqTransport *req_transport);
  void reset();
  int start();
  void stop();
  void wait();
  void destroy();
  void run1();

  int handle_gts_err_response(const ObGtsErrResponse &msg);
  int handle_gts_result(const uint64_t tenant_id, const int64_t queue_index, const int ts_type);
  int update_gts(const uint64_t tenant_id, const MonotonicTs srr, const int64_t gts, const int ts_type, bool &update);
  int delete_tenant(const uint64_t tenant_id);
public:
  int update_gts(const uint64_t tenant_id, const int64_t gts, bool &update);
  /*
  int update_local_trans_version(const uint64_t tenant_id, const int64_t version, bool &update);
  */

  //根据stc获取合适的gts值，如果条件不满足需要注册gts task，等异步回调
  int get_gts(const uint64_t tenant_id,
              const MonotonicTs stc,
              ObTsCbTask *task,
              share::SCN &scn,
              MonotonicTs &receive_gts_ts);
  //仅仅获取本地gts cache的最新值，但可能会失败，失败之后处理逻辑如下:
  //1. 如果task == NULL，说明调用者不需要异步回调，直接返回报错，由调用者处理
  //2. 如果task != NULL，需要注册异步回调任务
  int get_gts(const uint64_t tenant_id, ObTsCbTask *task, share::SCN &scn);
  int get_ts_sync(const uint64_t tenant_id, const int64_t timeout_ts,
      share::SCN &scn, bool &is_external_consistent);
  /*
  int get_local_trans_version(const uint64_t tenant_id,
                              const MonotonicTs stc,
                              ObTsCbTask *task,
                              int64_t &gts,
                              MonotonicTs &receive_gts_ts);
  int get_local_trans_version(const uint64_t tenant_id,
                              ObTsCbTask *task,
                              int64_t &gts);
                              */
  int wait_gts_elapse(const uint64_t tenant_id, const share::SCN &scn, ObTsCbTask *task,
      bool &need_wait);
  int wait_gts_elapse(const uint64_t tenant_id, const share::SCN &scn);
  int update_base_ts(const int64_t base_ts);
  int get_base_ts(int64_t &base_ts);
  bool is_external_consistent(const uint64_t tenant_id);
  static int get_cur_ts_type(const uint64_t tenant_id, int64_t &cur_ts_type);
  int refresh_gts_location(const uint64_t tenant_id);
public:
  TO_STRING_KV("ts_source", "GTS");
public:
  static ObTsMgr &get_instance();
private:
  static const int64_t TS_SOURCE_INFO_OBSOLETE_TIME = 120 * 1000 * 1000;
  static const int64_t TS_SOURCE_INFO_CACHE_NUM = 4096;
private:
  int get_ts_source_info_opt_(const uint64_t tenant_id, ObTsSourceInfoGuard &guard,
      const bool need_create_tenant, const bool need_update_access_ts);
  int get_ts_source_info_(const uint64_t tenant_id, ObTsSourceInfoGuard &guard,
      const bool need_create_tenant, const bool need_update_access_ts);
  void revert_ts_source_info_(ObTsSourceInfoGuard &guard);
  int add_tenant_(const uint64_t tenant_id);
  int delete_tenant_(const uint64_t tenant_id);
  int remove_dropped_tenant_(const uint64_t tenant_id);
  static ObTsMgr* &get_instance_inner();
private:
  bool is_inited_;
  bool is_running_;
  ObTsSourceInfoMap ts_source_info_map_;
  common::ObAddr server_;
  obrpc::ObGtsRpcProxy *gts_request_rpc_proxy_;
  ObGtsRequestRpc *gts_request_rpc_;
  ObLocationAdapter *location_adapter_;
  ObLocationAdapter location_adapter_def_;
  ObTsWorker ts_worker_;
  share::ObQSyncLock lock_;
  ObTsSourceInfo *ts_source_infos_[TS_SOURCE_INFO_CACHE_NUM];
};

class GetBaseTs
{
public:
  GetBaseTs() : base_ts_(0) {}
  bool operator()(const ObTsTenantInfo &tenant, ObTsSourceInfo *ts_source_info)
  {
    UNUSED(tenant);
    int ret = common::OB_SUCCESS;
    bool bool_ret = false;
    int64_t ts = 0;
    if (NULL == ts_source_info) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      ObGtsSource *gts_source = ts_source_info->get_gts_source();
      if (OB_FAIL(gts_source->get_base_ts(ts))) {
      } else {
        base_ts_ = (ts > base_ts_ ? ts : base_ts_);
        bool_ret = true;
      }
    }
    return bool_ret;
  }
  int64_t get_base_ts() const { return base_ts_; }
private:
  int64_t base_ts_;
};

class UpdateBaseTs
{
public:
  UpdateBaseTs(const int64_t base_ts) : base_ts_(base_ts) {}
  bool operator()(const ObTsTenantInfo &tenant, ObTsSourceInfo *ts_source_info)
  {
    UNUSED(tenant);
    int ret = common::OB_SUCCESS;
    bool bool_ret = false;
    if (NULL == ts_source_info) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      ObGtsSource *gts_source = ts_source_info->get_gts_source();
      if (OB_FAIL(gts_source->update_base_ts(base_ts_))) {
      } else {
        bool_ret = true;
      }
    }
    return bool_ret;
  }
private:
  int64_t base_ts_;
};

#define OB_TS_MGR (::oceanbase::transaction::ObTsMgr::get_instance())

}
}//end of namespace oceanbase

#endif //OCEANBASE_TRANSACTION_OB_TS_MGR_
