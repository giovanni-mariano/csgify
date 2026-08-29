#pragma once

// Append-only decision log writer. See docs/DECISION_LOG_AND_RECIPE.md.
//
// This is the MVP (step 1 of the plan in that doc):
// - Newline-delimited JSON, manifest record first, then decision records.
// - Sequential integer ids; content-addressed ids deferred to step 2.
// - No geometric fingerprints yet; deferred to step 3 (soft-match replay).
//
// Reused by session.cpp; not part of the installed public API.

#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine_model.hpp"  // ShapeFingerprint

namespace csgify {

// One parameter on a decision record. value is a JSON-encoded fragment
// (numbers without quotes, strings already wrapped and escaped). The caller
// uses the helpers below to construct entries safely.
struct DecisionParam {
    std::string name;
    std::string value_json;
};

DecisionParam param_string(const std::string& name, const std::string& value);
DecisionParam param_double(const std::string& name, double value);
DecisionParam param_bool(const std::string& name, bool value);
DecisionParam param_int(const std::string& name, long long value);

struct DecisionTarget {
    // "solid" | "face" | "model" | "document"
    std::string kind;
    // Shape id valid at the moment the decision was issued (e.g. "solid:5"
    // or "doc:1/solid:5"). Informational; not stable across replay.
    std::string shape_id;
    // Stable identity for primary match on replay. Empty for model-scoped
    // ops or when the target is not an XCAF-identified shape.
    std::string component_entry;
    // Soft-match key when component_entry can no longer be resolved on a
    // revised STEP. Only valid for solid targets.
    ShapeFingerprint fingerprint;
};

class DecisionLog {
public:
    // Opens path for writing and emits the manifest. Throws on open failure.
    DecisionLog(const std::string& path, const std::string& step_path);
    ~DecisionLog();

    DecisionLog(const DecisionLog&) = delete;
    DecisionLog& operator=(const DecisionLog&) = delete;

    // Append a decision record. `result` is one of "applied", "failed",
    // "skipped". `produced` lists shape ids the op created, or is empty.
    void record(const DecisionTarget& target,
                const std::string& op,
                const std::vector<DecisionParam>& params,
                const std::string& result,
                const std::vector<std::string>& produced = {});

    // Append an undo record referencing a prior decision by sequence number.
    void record_undo(std::size_t undoes_seq);

    const std::string& path() const { return path_; }
    std::size_t last_sequence() const { return seq_; }

private:
    void write_manifest(const std::string& step_path);

    std::string path_;
    std::ofstream out_;
    std::size_t seq_ = 0;        // last assigned seq; 0 == manifest
};

} // namespace csgify
