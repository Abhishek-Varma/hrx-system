// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0
//
// Lightweight, env-gated submit-path profiler for the amdxdna HAL driver.
//
// Enable at runtime with HRX_PROF=1. When disabled (default) every hook is a
// single predicted-not-taken branch and costs nothing. Totals are dumped to
// stderr at process exit (and can be dumped on demand via hrx_prof_dump()).
//
// This exists to answer "where does the per-token decode submit time go" by
// decomposing the host side of iree_hal_device_queue_execute (hrx_stream_flush)
// into cache-hit repatch vs control-code DMA sync vs kernel submit ioctl.

#ifndef IREE_HAL_DRIVERS_AMDXDNA_HRX_PROF_H_
#define IREE_HAL_DRIVERS_AMDXDNA_HRX_PROF_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hrx_prof_bucket_e {
  // One per hrx_stream_flush: the whole apply loop that replays the recorded
  // command buffer into a fresh direct command buffer and submits it.
  HRX_PROF_APPLY_LOOP = 0,
  // One per dispatch command replayed in the apply loop (context resolve +
  // normal_run/chain accumulate + submit). This is the per-dispatch cost.
  HRX_PROF_DISPATCH_PLAN,
  // Single-dispatch (non-chained) run path.
  HRX_PROF_NORMAL_RUN,
  // Chain-accumulation path (runlist children collected, finalized at end()).
  HRX_PROF_CHAIN_ACCUM,
  // Cache-HIT repatch of a retained single command: full control-code memcpy
  // into the mapped BO + dynamic-field/binding patch + metadata copies.
  HRX_PROF_SINGLE_HIT_REWRITE,
  // Cache-HIT repatch of a retained chain-child command (same work).
  HRX_PROF_CHAIN_HIT_REWRITE,
  // HOST_TO_DEVICE cache-maintenance sync of the control-code BO after rewrite.
  HRX_PROF_SYNC_H2D,
  // Kernel submission ioctl (native queue submit / submit_and_wait).
  HRX_PROF_SUBMIT_IOCTL,
  // Per-dispatch hardware-context resolution block in dispatch_plan (device
  // context-cache lookup / pin, borrowed-context path).
  HRX_PROF_CTX_RESOLVE,
  // The device-level get_or_create_context / pin_context call itself.
  HRX_PROF_GET_OR_CREATE_CTX,
  // The per-dispatch CU open (iree_hal_amdxdna_native_context_ref_open_cu).
  HRX_PROF_OPEN_CU,
  // Context-cache lookup loop (list walk + key compare) inside get_or_create.
  HRX_PROF_CTX_LOOKUP,
  // Context-cache miss path: eviction-budget scans + hardware context create.
  HRX_PROF_CTX_CREATE,
  // Synchronous portion of iree_hal_device_queue_execute on the CALLER's
  // thread (what the engine's layers_run.execute()/flush actually blocks on,
  // before the async worker runs the apply loop).
  HRX_PROF_QUEUE_EXEC_SYNC,
  // queue_execute_op_create: op alloc + completion batch + semaphore-list
  // clone (pure orchestration plumbing).
  HRX_PROF_OP_CREATE,
  // issue_ready: inline apply + submit on the caller thread (taken when the
  // wait semaphore is already satisfied, i.e. the serialized decode case).
  HRX_PROF_ISSUE_READY,
  // Fresh direct command buffer allocation per flush (queue_execute_apply).
  HRX_PROF_CB_CREATE,
  // completion_batch_submit: the ERT submit + (synchronous) completion path.
  HRX_PROF_COMPLETION_SUBMIT,
  // Per-flush teardown of the fresh direct command buffer (frees native
  // commands + BO refs built during apply). Created+destroyed every flush.
  HRX_PROF_CB_RELEASE,
  // iree_hal_command_buffer_end() on the target direct CB. In chain-
  // accumulation mode this is where the ERT_CMD_CHAIN is actually finalized
  // (built/patched/submitted) -- the true per-flush submit work.
  HRX_PROF_CB_END,
  // The genuine chain-cache-miss rebuild loop: make_npu_cmd for every child of
  // a multi-child chain (e.g. the fused 24-layer runlist) from scratch.
  HRX_PROF_CHAIN_BUILD_MISS,
  HRX_PROF_BUCKET_COUNT,
} hrx_prof_bucket_t;

// Records which chain-command-cache reuse tier a chain finalize landed in.
// Exactly one argument is 1. miss_build => the whole chain was rebuilt.
void hrx_prof_note_chain(int exact_hit, int template_hit, int device_hit,
                         int partial_hit, int miss_build);

// Records an executable_create decision: reuse hit, reuse miss (built+
// published), or not-eligible for reuse (built, not published).
void hrx_prof_note_exec(int reuse_hit, int reuse_miss, int not_eligible);

// Records a context-cache lookup outcome (hit=1 / miss=0).
void hrx_prof_note_ctx(int hit);

// Returns whether profiling is enabled (reads HRX_PROF once, then caches).
bool hrx_prof_enabled(void);

// Monotonic timestamp in nanoseconds. Returns 0 fast when profiling is off so
// callers can write: int64_t t = hrx_prof_t0(); ... hrx_prof_add(b, t);
int64_t hrx_prof_t0(void);

// Accumulates (now - start_ns) into bucket b and increments its call count.
// No-op when start_ns == 0 (i.e. profiling was off at hrx_prof_t0()).
void hrx_prof_add(hrx_prof_bucket_t b, int64_t start_ns);

// Adds observed bytes to bucket b (e.g. control-code bytes rewritten/synced).
void hrx_prof_add_bytes(hrx_prof_bucket_t b, uint64_t bytes);

// Marks one flush/submit boundary (one iree_hal_device_queue_execute).
void hrx_prof_mark_flush(void);

// Records, for one single-command cache hit, whether the control code, the
// bindings, and/or the recorded transaction template actually changed. This
// tells us how often a "hit" still pays the expensive control-code
// materialize+DMA-publish vs. taking the cheap no-op path.
void hrx_prof_note_rewrite(int control_changed, int bindings_changed,
                           int txn_changed);

// Dumps the accumulated table to stderr. Called automatically at exit.
void hrx_prof_dump(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_HAL_DRIVERS_AMDXDNA_HRX_PROF_H_
