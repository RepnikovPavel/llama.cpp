#include "llama-debug-stats.h"

#include "ggml.h"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

struct op_stats {
    uint64_t mults      = 0;
    uint64_t adds       = 0;
    uint64_t other_ops  = 0;
    uint64_t node_count = 0;
};

struct debug_stats {
    std::mutex mu;

    // read once from the environment
    bool        op_count_enabled     = false;
    bool        expert_trace_enabled = false;
    std::string expert_trace_path;

    bool        layer_dump_enabled = false;
    std::string layer_dump_path;

    std::map<std::string, std::map<std::string, op_stats>> backends; // backend name -> op name -> stats

    uint64_t resets = 0;
    std::chrono::steady_clock::time_point t_reset;

    FILE *   trace_file        = nullptr;
    bool     trace_open_failed = false;
    uint64_t trace_seq         = 0;

    FILE *   layer_dump_file        = nullptr;
    bool     layer_dump_open_failed = false;

    debug_stats() {
        const char * env_ops = getenv("LLAMA_DEBUG_OP_COUNT");
        op_count_enabled = env_ops && strcmp(env_ops, "1") == 0;

        const char * env_trace = getenv("LLAMA_DEBUG_EXPERT_TRACE");
        if (env_trace && env_trace[0] != '\0') {
            expert_trace_enabled = true;
            expert_trace_path    = env_trace;
        }

        const char * env_dump = getenv("LLAMA_DEBUG_LAYER_DUMP");
        if (env_dump && env_dump[0] != '\0') {
            layer_dump_enabled = true;
            layer_dump_path    = env_dump;
        }

        t_reset = std::chrono::steady_clock::now();
    }

    ~debug_stats() {
        if (trace_file) {
            fclose(trace_file);
        }
        if (layer_dump_file) {
            fclose(layer_dump_file);
        }
    }

    bool enabled() const {
        return op_count_enabled || expert_trace_enabled || layer_dump_enabled;
    }
};

debug_stats & stats() {
    static debug_stats instance;
    return instance;
}

// analytic per-node op cost, from tensor shapes known at graph build time
void node_cost(const ggml_tensor * t, uint64_t & mults, uint64_t & adds, uint64_t & other) {
    mults = 0;
    adds  = 0;
    other = 0;

    switch (t->op) {
        case GGML_OP_MUL_MAT:
            {
                const ggml_tensor * a = t->src[0]; // [K, N, ...]
                const ggml_tensor * b = t->src[1]; // [K, M, ...]
                const uint64_t K = a->ne[0];
                const uint64_t N = a->ne[1];
                const uint64_t M = b->ne[1] * b->ne[2] * b->ne[3];
                mults = M * N * K;
                adds  = K > 0 ? M * N * (K - 1) : 0;
            }
            break;
        case GGML_OP_MUL_MAT_ID:
            {
                // see ggml_mul_mat_id: as = [K, N, n_expert], b = [K, n_used, n_tokens], ids = [n_used, n_tokens]
                // only the selected experts are computed
                const ggml_tensor * as  = t->src[0];
                const ggml_tensor * ids = t->src[2];
                const uint64_t K    = as->ne[0];
                const uint64_t N    = as->ne[1];
                const uint64_t rows = ids->ne[0] * ids->ne[1];
                mults = rows * N * K;
                adds  = K > 0 ? rows * N * (K - 1) : 0;
            }
            break;
        case GGML_OP_ADD:
        case GGML_OP_ADD_ID:
        case GGML_OP_ADD1:
        case GGML_OP_ACC:
        case GGML_OP_SUB:
            adds = ggml_nelements(t);
            break;
        case GGML_OP_MUL:
        case GGML_OP_DIV:
            mults = ggml_nelements(t);
            break;
        case GGML_OP_SUM:
        case GGML_OP_SUM_ROWS:
        case GGML_OP_CUMSUM:
        case GGML_OP_MEAN:
            adds = ggml_nelements(t->src[0]);
            break;
        // layout-only ops: no arithmetic
        case GGML_OP_NONE:
        case GGML_OP_RESHAPE:
        case GGML_OP_VIEW:
        case GGML_OP_PERMUTE:
        case GGML_OP_TRANSPOSE:
        case GGML_OP_CONCAT:
        case GGML_OP_SET_ROWS:
            break;
        // memory movement, reported as other_ops
        case GGML_OP_CPY:
        case GGML_OP_DUP:
        case GGML_OP_CONT:
        case GGML_OP_GET_ROWS:
        case GGML_OP_SET:
            other = ggml_nelements(t);
            break;
        case GGML_OP_ARGSORT:
            other = ggml_nelements(t->src[0]); // comparisons
            break;
        default:
            other = ggml_nelements(t);
            break;
    }
}

void record_node(ggml_backend_sched_t sched, ggml_tensor * t) {
    uint64_t mults, adds, other;
    node_cost(t, mults, adds, other);

    ggml_backend_t backend = ggml_backend_sched_get_tensor_backend(sched, t);
    const char * bname = backend ? ggml_backend_name(backend) : "unknown";

    auto & s = stats();
    std::lock_guard<std::mutex> lock(s.mu);

    auto & st = s.backends[bname][ggml_op_name(t->op)];
    st.mults      += mults;
    st.adds       += adds;
    st.other_ops  += other;
    st.node_count += 1;
}

// the MoE expert selection node: llama-graph.cpp names the top-k view of the argsort
// result "ffn_moe_topk-<il>" (see cb(selected_experts, "ffn_moe_topk", il));
// ggml_argsort_top_k() is argsort + view, so the view gives us k = ne[0] and the
// contiguous argsort result t->src[0] holds all n_expert sorted ids per token
bool is_expert_selection_node(const ggml_tensor * t) {
    return t->op == GGML_OP_VIEW &&
           strncmp(t->name, "ffn_moe_topk-", 13) == 0 &&
           t->src[0] && t->src[0]->op == GGML_OP_ARGSORT;
}

void trace_experts(const ggml_tensor * t) {
    auto & s = stats();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.trace_file) {
        if (s.trace_open_failed) {
            return;
        }
        s.trace_file = fopen(s.expert_trace_path.c_str(), "a");
        if (!s.trace_file) {
            s.trace_open_failed = true;
            fprintf(stderr, "%s: failed to open expert trace file '%s'\n", __func__, s.expert_trace_path.c_str());
            return;
        }
    }

    const ggml_tensor * sorted = t->src[0]; // [n_expert, n_tokens] i32, contiguous
    if (sorted->type != GGML_TYPE_I32 || !ggml_is_contiguous(sorted)) {
        return;
    }

    const int64_t k        = t->ne[0];
    const int64_t n_tokens = t->ne[1];
    const int64_t n_expert = sorted->ne[0];

    std::vector<int32_t> data(ggml_nelements(sorted));
    ggml_backend_tensor_get(sorted, data.data(), 0, ggml_nbytes(sorted));

    const char * dash = strrchr(t->name, '-');
    const int layer = dash ? atoi(dash + 1) : -1;

    std::string line;
    line.reserve(64 + 8 * k * n_tokens);

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"seq\": %" PRIu64 ", \"layer\": %d, \"n_tokens\": %" PRId64 ", \"experts\": [",
            s.trace_seq++, layer, n_tokens);
    line += buf;

    for (int64_t j = 0; j < n_tokens; j++) {
        for (int64_t e = 0; e < k && e < n_expert; e++) {
            snprintf(buf, sizeof(buf), "%s%d", (j == 0 && e == 0) ? "" : ", ", data[j*n_expert + e]);
            line += buf;
        }
    }
    line += "]}\n";

    fwrite(line.data(), 1, line.size(), s.trace_file);
    fflush(s.trace_file);
}

// layer output nodes: the model build functions name the residual state at the
// end of layer <il> "l_out-<il>" (deepseek2, dflash) or "l_last-<il>" (deepseek4),
// and the final norm "result_norm" (see the cb() calls in src/models/)
bool is_layer_dump_node(const ggml_tensor * t) {
    // skip auto-suffixed views like "l_last-3 (reshaped)" - same data as the base node
    if (strchr(t->name, '(')) {
        return false;
    }
    return strncmp(t->name, "l_out-", 6) == 0 ||
           strncmp(t->name, "l_last-", 7) == 0 ||
           strcmp(t->name, "result_norm") == 0;
}

// binary record layout (all fields little-endian, no padding):
//   char     name[64]   - node name, zero-padded
//   uint32_t type       - ggml_type
//   uint32_t n_dims
//   int64_t  ne[4]      - dimensions
//   uint64_t nbytes     - payload size
//   uint8_t  payload[]  - raw tensor data (contiguous)
// the file starts with the 8-byte magic "DSBLAYR1"
struct layer_dump_header {
    char     name[64];
    uint32_t type;
    uint32_t n_dims;
    int64_t  ne[GGML_MAX_DIMS];
    uint64_t nbytes;
};

void dump_layer(const ggml_tensor * t) {
    auto & s = stats();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.layer_dump_file) {
        if (s.layer_dump_open_failed) {
            return;
        }
        s.layer_dump_file = fopen(s.layer_dump_path.c_str(), "wb");
        if (!s.layer_dump_file) {
            s.layer_dump_open_failed = true;
            fprintf(stderr, "%s: failed to open layer dump file '%s'\n", __func__, s.layer_dump_path.c_str());
            return;
        }
        fwrite("DSBLAYR1", 1, 8, s.layer_dump_file);
    }

    if (!ggml_is_contiguous(t)) {
        fprintf(stderr, "%s: skipping non-contiguous node '%s'\n", __func__, t->name);
        return;
    }

    layer_dump_header h;
    memset(&h, 0, sizeof(h));
    snprintf(h.name, sizeof(h.name), "%s", t->name);
    h.type   = t->type;
    h.n_dims = ggml_n_dims(t);
    for (int i = 0; i < GGML_MAX_DIMS; i++) {
        h.ne[i] = t->ne[i];
    }
    h.nbytes = ggml_nbytes(t);

    std::vector<uint8_t> data(h.nbytes);
    ggml_backend_tensor_get(t, data.data(), 0, h.nbytes);

    fwrite(&h, sizeof(h), 1, s.layer_dump_file);
    fwrite(data.data(), 1, data.size(), s.layer_dump_file);
    fflush(s.layer_dump_file);
}

void json_escape(std::string & out, const std::string & s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:   out += c;      break;
        }
    }
}

} // namespace

bool llama_debug_stats_enabled() {
    return stats().enabled();
}

bool llama_debug_stats_op_count_enabled() {
    return stats().op_count_enabled;
}

bool llama_debug_stats_expert_trace_enabled() {
    return stats().expert_trace_enabled;
}

bool llama_debug_stats_layer_dump_enabled() {
    return stats().layer_dump_enabled;
}

std::string llama_debug_stats_to_json() {
    auto & s = stats();
    std::lock_guard<std::mutex> lock(s.mu);

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t_reset).count();

    std::string out;
    out.reserve(4096);

    char buf[256];
    snprintf(buf, sizeof(buf),
            "{\"enabled\": %s, \"op_count_enabled\": %s, \"expert_trace_enabled\": %s, \"backends\": {",
            s.enabled() ? "true" : "false",
            s.op_count_enabled ? "true" : "false",
            s.expert_trace_enabled ? "true" : "false");
    out += buf;

    uint64_t tot_mults = 0, tot_adds = 0, tot_other = 0;

    bool first_backend = true;
    for (const auto & be : s.backends) {
        if (!first_backend) {
            out += ", ";
        }
        first_backend = false;

        out += "\"";
        json_escape(out, be.first);
        out += "\": {\"ops\": {";

        bool first_op = true;
        for (const auto & op : be.second) {
            if (!first_op) {
                out += ", ";
            }
            first_op = false;

            out += "\"";
            json_escape(out, op.first);
            out += "\": ";

            snprintf(buf, sizeof(buf), "{\"mults\": %" PRIu64 ", \"adds\": %" PRIu64 ", \"other_ops\": %" PRIu64 ", \"nodes\": %" PRIu64 "}",
                    op.second.mults, op.second.adds, op.second.other_ops, op.second.node_count);
            out += buf;

            tot_mults += op.second.mults;
            tot_adds  += op.second.adds;
            tot_other += op.second.other_ops;
        }
        out += "}}";
    }

    snprintf(buf, sizeof(buf), "}, \"totals\": {\"mults\": %" PRIu64 ", \"adds\": %" PRIu64 ", \"other_ops\": %" PRIu64 "}, \"expert_trace_path\": \"",
            tot_mults, tot_adds, tot_other);
    out += buf;
    json_escape(out, s.expert_trace_path);

    snprintf(buf, sizeof(buf), "\", \"resets\": %" PRIu64 ", \"seconds_since_reset\": %.3f}", s.resets, seconds);
    out += buf;

    return out;
}

void llama_debug_stats_reset() {
    auto & s = stats();
    std::lock_guard<std::mutex> lock(s.mu);

    s.backends.clear();
    s.resets += 1;
    s.t_reset = std::chrono::steady_clock::now();
}

bool llama_debug_eval_callback(ggml_tensor * t, bool ask, void * user_data) {
    auto * d = static_cast<llama_debug_eval_data *>(user_data);

    if (ask) {
        bool need = false;

        if (llama_debug_stats_expert_trace_enabled() && is_expert_selection_node(t)) {
            need = true;
        }

        if (llama_debug_stats_layer_dump_enabled() && is_layer_dump_node(t)) {
            need = true;
        }

        if (llama_debug_stats_op_count_enabled()) {
            record_node(d->sched, t);
        }

        d->chained_need = d->chained_cb ? d->chained_cb(t, true, d->chained_user_data) : false;

        return need || d->chained_need;
    }

    if (llama_debug_stats_expert_trace_enabled() && is_expert_selection_node(t)) {
        trace_experts(t);
    }

    if (llama_debug_stats_layer_dump_enabled() && is_layer_dump_node(t)) {
        dump_layer(t);
    }

    bool ok = true;
    if (d->chained_need && d->chained_cb) {
        ok = d->chained_cb(t, false, d->chained_user_data);
    }
    return ok;
}
