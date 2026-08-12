#pragma once

#include "ggml-backend.h"

#include <string>

// optional debug instrumentation, enabled via environment variables:
//   LLAMA_DEBUG_OP_COUNT=1        - count mults/adds/other ops per graph compute, by backend and op type
//   LLAMA_DEBUG_EXPERT_TRACE=<p>  - append selected MoE experts per layer per token to the JSONL file <p>
//   LLAMA_DEBUG_LAYER_DUMP=<p>  - append per-layer hidden states (nodes "l_out-<il>" / "l_last-<il>"
//                                 and "result_norm") to the binary file <p>; record layout in dump_layer()
//
// note: enabling either installs a scheduler eval callback, which forces per-batch backend
// synchronization in ggml_backend_sched - do not enable in performance-sensitive runs

// user_data for llama_debug_eval_callback
struct llama_debug_eval_data {
    ggml_backend_sched_t              sched             = nullptr;
    ggml_backend_sched_eval_callback  chained_cb        = nullptr;
    void *                            chained_user_data = nullptr;
    bool                              chained_need      = false; // result of the chained callback for the last asked node
};

bool llama_debug_stats_enabled();
bool llama_debug_stats_op_count_enabled();
bool llama_debug_stats_expert_trace_enabled();
bool llama_debug_stats_layer_dump_enabled();

std::string llama_debug_stats_to_json();
void        llama_debug_stats_reset();

bool llama_debug_eval_callback(ggml_tensor * t, bool ask, void * user_data);
