// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2011-2024 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef FREICOIN_UTIL_TRACE_H
#define FREICOIN_UTIL_TRACE_H

#include <config/freicoin-config.h> // IWYU pragma: keep

#ifdef ENABLE_TRACING

// Setting SDT_USE_VARIADIC lets systemtap (sys/sdt.h) know that we want to use
// the optional variadic macros to define tracepoints.
#define SDT_USE_VARIADIC 1

// Setting _SDT_HAS_SEMAPHORES let's systemtap (sys/sdt.h) know that we want to
// use the optional semaphore feature for our tracepoints. This feature allows
// us to check if something is attached to a tracepoint. We only want to prepare
// some potentially expensive tracepoint arguments, if the tracepoint is being
// used. Here, an expensive argument preparation could, for example, be
// calculating a hash or serialization of a data structure.
#define _SDT_HAS_SEMAPHORES 1

// Used to define a counting semaphore for a tracepoint. This semaphore is
// automatically incremented by tracing frameworks (bpftrace, bcc, libbpf, ...)
// upon attaching to the tracepoint and decremented when detaching. This needs
// to be a global variable. It's placed in the '.probes' ELF section.
#define TRACEPOINT_SEMAPHORE(context, event) \
    unsigned short context##_##event##_semaphore __attribute__((section(".probes")))

#include <sys/sdt.h>

// Returns true if something is attached to the tracepoint.
#define TRACEPOINT_ACTIVE(context, event) (context##_##event##_semaphore > 0)

// A USDT tracepoint with one to twelve arguments. It's checked that the
// tracepoint is active before preparing its arguments.
#define TRACEPOINT(context, event, ...)                                         \
    do {                                                                        \
        if (TRACEPOINT_ACTIVE(context, event)) {                                \
            STAP_PROBEV(context, event __VA_OPT__(, ) __VA_ARGS__);             \
        }                                                                       \
    } while(0)

#else

#define TRACEPOINT_SEMAPHORE(context, event)
#define TRACEPOINT_ACTIVE(context, event) false
#define TRACEPOINT(context, ...)

#endif // ENABLE_TRACING


#endif // FREICOIN_UTIL_TRACE_H
