#include "decision_log.hpp"

#include "csgify/engine.hpp"  // occt_version()
#include "fnv.hpp"
#include "operation_registry.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <stdexcept>

namespace csgify {
namespace {

// Local JSON helpers — small enough that pulling report.cpp's namespace in
// would not be worth the coupling.

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"':  out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

std::string quote(const std::string& value) {
    return std::string("\"") + json_escape(value) + "\"";
}

std::string number(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

std::string iso8601_utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto sec = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::size_t latest_seq_in_log(const std::string& path) {
    std::ifstream in(path);
    std::size_t latest = 0;
    std::string line;
    while (std::getline(in, line)) {
        const std::string marker = "\"seq\":";
        const std::size_t pos = line.find(marker);
        if (pos == std::string::npos) continue;
        std::size_t i = pos + marker.size();
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t value = 0;
        bool have = false;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            have = true;
            value = value * 10 + static_cast<std::size_t>(line[i] - '0');
            ++i;
        }
        if (have) latest = std::max(latest, value);
    }
    return latest;
}

bool file_has_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    return in && in.tellg() > 0;
}

} // namespace

DecisionParam param_string(const std::string& name, const std::string& value) {
    return {name, quote(value)};
}

DecisionParam param_double(const std::string& name, double value) {
    return {name, number(value)};
}

DecisionParam param_bool(const std::string& name, bool value) {
    return {name, value ? "true" : "false"};
}

DecisionParam param_int(const std::string& name, long long value) {
    std::ostringstream out;
    out << value;
    return {name, out.str()};
}

DecisionLog::DecisionLog(const std::string& path, const std::string& step_path)
    : path_(path) {
    const bool append_existing = file_has_content(path);
    seq_ = latest_seq_in_log(path);
    out_.open(path, std::ios::out | std::ios::app);
    if (!out_.is_open()) {
        throw std::runtime_error("could not open decision log: " + path);
    }
    if (!append_existing) {
        write_manifest(step_path);
    }
}

DecisionLog::~DecisionLog() {
    if (out_.is_open()) {
        out_.close();
    }
}

void DecisionLog::write_manifest(const std::string& step_path) {
    // TODO: include csgify_version once the build exposes one.
    std::string digest;
    if (!step_path.empty()) {
        const std::string raw = fnv1a128_file_hex(step_path);
        if (!raw.empty()) {
            digest = "fnv1a128:" + raw;
        }
    }
    out_ << "{"
         << "\"kind\":\"manifest\","
         << "\"schema\":\"csgify.decision-log\","
         << "\"schema_version\":1,"
         << "\"seq\":0,"
         << "\"csgify_version\":\"unversioned\","
         << "\"occt_version\":" << quote(occt_version()) << ","
         << "\"step_path\":" << quote(step_path) << ","
         << "\"step_digest\":" << quote(digest) << ","
         << "\"created_at\":" << quote(iso8601_utc_now())
         << "}\n";
    out_.flush();
}

void DecisionLog::record(const DecisionTarget& target,
                         const std::string& op,
                         const std::vector<DecisionParam>& params,
                         const std::string& result,
                         const std::vector<std::string>& produced) {
    if (!out_.is_open()) {
        return;
    }
    ++seq_;
    const OperationDescriptor* descriptor = find_operation(op);

    out_ << "{"
         << "\"kind\":\"decision\","
         << "\"seq\":" << seq_ << ","
         << "\"target\":{"
         <<   "\"kind\":" << quote(target.kind) << ","
         <<   "\"shape_id\":" << quote(target.shape_id) << ","
         <<   "\"component_entry\":" << quote(target.component_entry);
    if (target.fingerprint.valid) {
        out_ << ","
             << "\"fingerprint\":{"
             <<   "\"volume\":"        << number(target.fingerprint.volume) << ","
             <<   "\"surface_area\":"  << number(target.fingerprint.surface_area) << ","
             <<   "\"bbox_diagonal\":" << number(target.fingerprint.bbox_diagonal) << ","
             <<   "\"face_count\":"    << target.fingerprint.face_count << ","
             <<   "\"centroid\":["     << number(target.fingerprint.centroid_x) << ","
             <<                           number(target.fingerprint.centroid_y) << ","
             <<                           number(target.fingerprint.centroid_z) << "]"
             << "}";
    }
    out_ << "},"
         << "\"op\":" << quote(op) << ","
         << "\"operation_version\":"
         << (descriptor ? descriptor->version : 0) << ","
         << "\"params\":{";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) out_ << ",";
        out_ << quote(params[i].name) << ":" << params[i].value_json;
    }
    out_ << "},"
         << "\"result\":" << quote(result) << ","
         << "\"produced\":[";
    for (std::size_t i = 0; i < produced.size(); ++i) {
        if (i) out_ << ",";
        out_ << quote(produced[i]);
    }
    out_ << "],"
         << "\"issued_at\":" << quote(iso8601_utc_now())
         << "}\n";
    out_.flush();
}

void DecisionLog::record_undo(std::size_t undoes_seq) {
    if (!out_.is_open()) {
        return;
    }
    ++seq_;

    out_ << "{"
         << "\"kind\":\"undo\","
         << "\"seq\":" << seq_ << ","
         << "\"undoes_seq\":" << undoes_seq << ","
         << "\"issued_at\":" << quote(iso8601_utc_now())
         << "}\n";
    out_.flush();
}

} // namespace csgify
