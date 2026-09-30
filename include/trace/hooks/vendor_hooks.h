/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TRACE_HOOKS_VENDOR_HOOKS_H
#define __TRACE_HOOKS_VENDOR_HOOKS_H

#include <linux/tracepoint.h>

/* Android hook headers use these wrappers; 4.4 needs plain arguments. */
#ifndef TP_PROTO
#define TP_PROTO(...) __VA_ARGS__
#endif
#ifndef TP_ARGS
#define TP_ARGS(...) __VA_ARGS__
#endif

/* Compatibility layer for the 4.4 tracepoint API. */
#define DECLARE_HOOK(name, proto, args) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))

#define DECLARE_RESTRICTED_HOOK(name, proto, args, cond) \
	DECLARE_TRACE_CONDITION(name, PARAMS(proto), PARAMS(args), cond)

#endif /* __TRACE_HOOKS_VENDOR_HOOKS_H */
