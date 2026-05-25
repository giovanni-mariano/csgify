#pragma once

#include "cad2csg/engine.hpp"

#include <cstddef>
#include <string>

namespace cad2csg {

struct CadModel;

struct DecisionPackOptions {
    std::string output_dir;
    std::string target_id;
    std::size_t max_packets = 0; // 0 means no explicit limit.
};

struct DecisionPackResult {
    bool ok = false;
    std::size_t packets_written = 0;
    std::string error;
};

DecisionPackResult write_decision_packs(const ConversionReport& report,
                                        const DecisionPackOptions& options);

DecisionPackResult write_decision_packs(const ConversionReport& report,
                                        const CadModel& model,
                                        const DecisionPackOptions& options);

} // namespace cad2csg
