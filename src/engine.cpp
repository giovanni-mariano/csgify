// Public-API entry points that aren't covered by a stage TU:
// - AleaSystem RAII
// - occt_version()
// - ConversionReport::ok()
// - compare_step_volumes()
// - export_mcnp()
//
// Every other public function is implemented in the corresponding stage TU:
//   import.cpp, analyze.cpp, cleanup.cpp, emit.cpp, report.cpp.

#include "engine_model.hpp"
#include "engine_internal.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

#include <Standard_Version.hxx>

#include <alea.h>
#include <alea_mcnp.h>

namespace csgify {

const char* occt_version() {
    return OCC_VERSION_COMPLETE;
}

bool ConversionReport::ok() const {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (detail::is_error(diagnostic)) {
            return false;
        }
    }
    return true;
}

AleaSystem::AleaSystem() : system_(alea_create()) {
    if (!system_) {
        throw std::runtime_error("alea_create failed");
    }
}

AleaSystem::~AleaSystem() {
    alea_destroy(system_);
}

AleaSystem::AleaSystem(AleaSystem&& other) noexcept : system_(other.system_) {
    other.system_ = nullptr;
}

AleaSystem& AleaSystem::operator=(AleaSystem&& other) noexcept {
    if (this != &other) {
        alea_destroy(system_);
        system_ = other.system_;
        other.system_ = nullptr;
    }
    return *this;
}

alea_system_t* AleaSystem::release() {
    alea_system_t* released = system_;
    system_ = nullptr;
    return released;
}

VolumeComparison compare_step_volumes(const std::string& original_path,
                                      const std::string& candidate_path,
                                      double absolute_tolerance,
                                      double relative_tolerance,
                                      const ConversionOptions& options) {
    VolumeComparison comparison;
    comparison.absolute_tolerance = absolute_tolerance;
    comparison.relative_tolerance = relative_tolerance;
    comparison.original = inspect_step_file(original_path, options);
    comparison.candidate = inspect_step_file(candidate_path, options);
    comparison.absolute_delta =
        std::abs(comparison.candidate.total_volume - comparison.original.total_volume);
    if (std::abs(comparison.original.total_volume) > detail::kEps) {
        comparison.relative_delta = comparison.absolute_delta
            / std::abs(comparison.original.total_volume);
    } else {
        comparison.relative_delta = comparison.absolute_delta > detail::kEps ? 1.0 : 0.0;
    }

    const bool abs_ok = absolute_tolerance <= 0.0
        || comparison.absolute_delta <= absolute_tolerance;
    const bool rel_ok = relative_tolerance <= 0.0
        || comparison.relative_delta <= relative_tolerance;
    comparison.within_tolerance = abs_ok && rel_ok;
    return comparison;
}

int export_mcnp(alea_system_t* system, const std::string& output_path) {
    return mcnp_export_system(system, output_path.c_str());
}

} // namespace csgify
