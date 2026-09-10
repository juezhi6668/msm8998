// SPDX-License-Identifier: GPL-2.0
/* Generate the 4.4 tracepoint objects used by the Millet compatibility hooks. */
#define CREATE_TRACE_POINTS
#include <trace/hooks/signal.h>
#include <trace/hooks/binder.h>

DEFINE_TRACE(android_vh_do_send_sig_info);

DEFINE_TRACE(android_vh_binder_transaction_init);
DEFINE_TRACE(android_vh_binder_priority_skip);
DEFINE_TRACE(android_vh_binder_set_priority);
DEFINE_TRACE(android_vh_binder_restore_priority);
DEFINE_TRACE(android_vh_binder_wakeup_ilocked);
DEFINE_TRACE(android_vh_binder_wait_for_work);
DEFINE_TRACE(android_vh_sync_txn_recvd);
DEFINE_TRACE(android_vh_binder_alloc_new_buf_locked);
DEFINE_TRACE(android_vh_binder_reply);
DEFINE_TRACE(android_vh_binder_trans);
DEFINE_TRACE(android_vh_binder_preset);
DEFINE_TRACE(android_vh_binder_proc_transaction);
DEFINE_TRACE(android_vh_binder_new_ref);
DEFINE_TRACE(android_vh_binder_del_ref);
DEFINE_TRACE(android_vh_binder_print_transaction_info);
