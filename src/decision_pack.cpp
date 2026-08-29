#include "decision_pack.hpp"

#include "engine_internal.hpp"
#include "engine_model.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <Graphic3d_GraphicDriverFactory.hxx>
#include <OpenGl_GraphicDriverFactory.hxx>
#include <PrsMgr_DisplayStatus.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <Xw_Window.hxx>

namespace csgify {
namespace {

struct TargetInfo {
    std::string id;
    std::string label;
    std::string name;
    std::string hierarchy_path;
    BBox bbox;
    double volume = 0.0;
    double area = 0.0;
    std::size_t face_count = 0;
};

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
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
            break;
        }
    }
    return out.str();
}

std::string html_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::string slug_char(char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')) {
        return std::string(1, ch);
    }
    return "-";
}

std::string slug(const std::string& value) {
    std::string out;
    for (char ch : value) {
        out += slug_char(ch);
    }
    while (out.find("--") != std::string::npos) {
        out.replace(out.find("--"), 2, "-");
    }
    while (!out.empty() && out.front() == '-') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "decision" : out;
}

bool write_text_file(const std::filesystem::path& path,
                     const std::string& text,
                     std::string* error) {
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "could not open " + path.string();
        return false;
    }
    out << text;
    if (!out) {
        if (error) *error = "could not write " + path.string();
        return false;
    }
    return true;
}

bool ensure_occt_driver_factory() {
    if (Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull()) {
        Graphic3d_GraphicDriverFactory::RegisterFactory(
            new OpenGl_GraphicDriverFactory(),
            true);
    }
    return !Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull();
}

bool write_occt_overview_png(const CadModel& model,
                             const std::filesystem::path& path,
                             std::string* error) {
    if (model.root.IsNull()) {
        if (error) *error = "model is empty";
        return false;
    }
    try {
        if (!ensure_occt_driver_factory()) {
            if (error) *error = "OCCT OpenGL driver factory is unavailable";
            return false;
        }

        occ::handle<Aspect_DisplayConnection> display =
            new Aspect_DisplayConnection();
        occ::handle<Graphic3d_GraphicDriver> driver =
            Graphic3d_GraphicDriverFactory::DefaultDriverFactory()->CreateDriver(display);
        if (driver.IsNull()) {
            if (error) *error = "OCCT could not create Graphic3d driver";
            return false;
        }
        driver->SetVerticalSync(false);

        occ::handle<V3d_Viewer> viewer = new V3d_Viewer(driver);
        viewer->SetDefaultBackgroundColor(Quantity_NOC_WHITE);
        viewer->SetDefaultLights();
        viewer->SetLightOn();

        occ::handle<AIS_InteractiveContext> context =
            new AIS_InteractiveContext(viewer);
        occ::handle<V3d_View> view = viewer->CreateView();
        occ::handle<Xw_Window> window =
            new Xw_Window(driver->GetDisplayConnection(),
                          "csgify decision packet",
                          0,
                          0,
                          1000,
                          760);
        window->SetVirtual(true);
        window->Map();
        view->SetWindow(window);

        std::size_t solid_index = 0;
        for (TopExp_Explorer exp(model.root, TopAbs_SOLID); exp.More(); exp.Next()) {
            ++solid_index;
            const std::string id = detail::solid_id(solid_index);
            if (detail::contains_id(model.removed_solid_ids, id)) {
                continue;
            }
            occ::handle<AIS_Shape> shape =
                new AIS_Shape(exp.Current());
            shape->SetColor(Quantity_Color(0.55, 0.68, 0.78, Quantity_TOC_RGB));
            context->Display(shape, AIS_Shaded, -1, false, PrsMgr_DisplayStatus_None);
        }

        context->UpdateCurrentViewer();
        view->SetProj(V3d_XposYnegZpos);
        view->FitAll(0.08, false);
        view->ZFitAll();
        view->Redraw();
        if (!view->Dump(path.string().c_str())) {
            if (error) *error = "OCCT V3d_View::Dump failed";
            return false;
        }
        return true;
    } catch (const Standard_Failure& failure) {
        if (error) {
            *error = std::string("OCCT viewer failed: ") + failure.what();
        }
        return false;
    } catch (const std::exception& failure) {
        if (error) *error = std::string("OCCT viewer failed: ") + failure.what();
        return false;
    }
}

bool find_target(const ConversionReport& report,
                 const std::string& target_id,
                 TargetInfo* out) {
    for (const AnalyzedSolid& solid : report.solids) {
        if (solid.id == target_id) {
            *out = TargetInfo{solid.id, solid.label, solid.name,
                              solid.hierarchy_path, solid.bbox,
                              solid.volume, 0.0, solid.face_count};
            return true;
        }
    }
    for (const AnalyzedFace& face : report.faces) {
        if (face.id == target_id) {
            *out = TargetInfo{face.id, face.label, face.name,
                              face.hierarchy_path, face.bbox,
                              0.0, face.area, 0};
            return true;
        }
    }
    for (const AnalyzedDecomposedPiece& piece : report.decomposed_pieces) {
        if (piece.id == target_id) {
            *out = TargetInfo{piece.id, piece.label, {}, {},
                              piece.bbox, piece.volume, 0.0,
                              piece.face_count};
            return true;
        }
    }
    return false;
}

double bbox_span_x(const BBox& bbox) { return bbox.xmax - bbox.xmin; }
double bbox_span_y(const BBox& bbox) { return bbox.ymax - bbox.ymin; }
double bbox_span_z(const BBox& bbox) { return bbox.zmax - bbox.zmin; }

std::string write_metric_line(const DecisionMetric& metric) {
    std::ostringstream out;
    out << "- `" << metric.name << "`: " << std::setprecision(8)
        << metric.value;
    if (!metric.unit.empty()) {
        out << " " << metric.unit;
    }
    out << "\n";
    return out.str();
}

bool metric_value(const DecisionRequest& request,
                  const std::string& name,
                  double* value_out) {
    for (const DecisionMetric& metric : request.metrics) {
        if (metric.name == name) {
            *value_out = metric.value;
            return true;
        }
    }
    return false;
}

TargetInfo synthetic_target_for_request(const DecisionRequest& request) {
    TargetInfo target;
    target.id = request.target_id;
    target.label = request.target_id;
    if (request.kind == "hole_fill") {
        double radius = 0.0;
        double length = 0.0;
        if (metric_value(request, "radius", &radius)
            && metric_value(request, "length", &length)
            && radius > 0.0
            && length > 0.0) {
            target.bbox.valid = true;
            target.bbox.xmin = -radius;
            target.bbox.ymin = -radius;
            target.bbox.zmin = 0.0;
            target.bbox.xmax = radius;
            target.bbox.ymax = radius;
            target.bbox.zmax = length;
        }
    }
    return target;
}

std::string svg_for_request(const DecisionRequest& request,
                            const TargetInfo& target) {
    constexpr double width = 900.0;
    constexpr double height = 620.0;
    constexpr double pad = 80.0;

    const bool valid = target.bbox.valid;
    const double sx = valid ? std::max(1.0e-12, bbox_span_x(target.bbox)) : 1.0;
    const double sy = valid ? std::max(1.0e-12, bbox_span_y(target.bbox)) : 1.0;
    const double sz = valid ? std::max(1.0e-12, bbox_span_z(target.bbox)) : 1.0;
    const double max_s = std::max({sx, sy, sz});

    const double scale = std::min(210.0 / max_s, 220.0 / max_s);
    const double wxy = sx * scale;
    const double hxy = sy * scale;
    const double wxz = sx * scale;
    const double hxz = sz * scale;
    const double wyz = sy * scale;
    const double hyz = sz * scale;

    auto rect = [](double x, double y, double w, double h,
                   const std::string& fill,
                   const std::string& stroke) {
        std::ostringstream out;
        out << "<rect x=\"" << x << "\" y=\"" << y
            << "\" width=\"" << std::max(1.0, w)
            << "\" height=\"" << std::max(1.0, h)
            << "\" rx=\"4\" fill=\"" << fill
            << "\" stroke=\"" << stroke
            << "\" stroke-width=\"2\" />\n";
        return out.str();
    };

    std::ostringstream out;
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
        << "\" height=\"" << height << "\" viewBox=\"0 0 " << width
        << " " << height << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#f7f7f2\" />\n";
    out << "<text x=\"40\" y=\"44\" font-family=\"monospace\" font-size=\"22\" "
        << "fill=\"#1f2933\">" << html_escape(request.kind)
        << " " << html_escape(request.target_id) << "</text>\n";
    out << "<text x=\"40\" y=\"74\" font-family=\"monospace\" font-size=\"14\" "
        << "fill=\"#52606d\">" << html_escape(request.reason) << "</text>\n";

    if (!valid) {
        out << "<text x=\"40\" y=\"180\" font-family=\"monospace\" "
            << "font-size=\"18\" fill=\"#9b1c1c\">No bounding box available "
            << "for this target.</text>\n";
    } else {
        out << "<g opacity=\"0.92\">\n";
        out << rect(70, 145 + (220 - hxy) * 0.5, wxy, hxy,
                    "#77c8b5", "#1f6f64");
        out << "<text x=\"70\" y=\"130\" font-family=\"monospace\" "
            << "font-size=\"14\" fill=\"#1f2933\">XY view</text>\n";

        out << rect(340, 145 + (220 - hxz) * 0.5, wxz, hxz,
                    "#f2b880", "#9a5b16");
        out << "<text x=\"340\" y=\"130\" font-family=\"monospace\" "
            << "font-size=\"14\" fill=\"#1f2933\">XZ view</text>\n";

        out << rect(610, 145 + (220 - hyz) * 0.5, wyz, hyz,
                    "#9db7e8", "#365899");
        out << "<text x=\"610\" y=\"130\" font-family=\"monospace\" "
            << "font-size=\"14\" fill=\"#1f2933\">YZ view</text>\n";
        out << "</g>\n";

        out << "<text x=\"40\" y=\"430\" font-family=\"monospace\" "
            << "font-size=\"15\" fill=\"#1f2933\">bbox spans: x="
            << std::setprecision(6) << sx << " y=" << sy << " z=" << sz
            << "</text>\n";
    }

    out << "<text x=\"40\" y=\"470\" font-family=\"monospace\" "
        << "font-size=\"15\" fill=\"#1f2933\">recommended: "
        << html_escape(request.recommended_action) << "</text>\n";
    out << "<text x=\"40\" y=\"500\" font-family=\"monospace\" "
        << "font-size=\"15\" fill=\"#1f2933\">summary: "
        << html_escape(request.summary).substr(0, 96) << "</text>\n";

    out << "<text x=\"40\" y=\"570\" font-family=\"monospace\" "
        << "font-size=\"12\" fill=\"#7b8794\">This is a lightweight "
        << "decision view. Full OCCT-shaded rendering is a later layer.</text>\n";

    if (request.kind == "hole_fill") {
        out << "<text x=\"470\" y=\"470\" font-family=\"monospace\" "
            << "font-size=\"15\" fill=\"#1f2933\">question: fill this cavity "
            << "with host material, or leave it void?</text>\n";
    }
    out << "</svg>\n";
    return out.str();
}

std::string manifest_json(const DecisionRequest& request,
                          const TargetInfo& target,
                          const std::string& image_path,
                          const std::string& overview_path = {}) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"decision_id\": \"" << json_escape(request.id) << "\",\n";
    out << "  \"kind\": \"" << json_escape(request.kind) << "\",\n";
    out << "  \"target_id\": \"" << json_escape(request.target_id) << "\",\n";
    out << "  \"reason\": \"" << json_escape(request.reason) << "\",\n";
    out << "  \"recommended_action\": \""
        << json_escape(request.recommended_action) << "\",\n";
    out << "  \"summary\": \"" << json_escape(request.summary) << "\",\n";
    out << "  \"target\": {\n";
    out << "    \"id\": \"" << json_escape(target.id) << "\",\n";
    out << "    \"label\": \"" << json_escape(target.label) << "\",\n";
    out << "    \"name\": \"" << json_escape(target.name) << "\",\n";
    out << "    \"hierarchy_path\": \"" << json_escape(target.hierarchy_path) << "\"\n";
    out << "  },\n";
    out << "  \"images\": {\n";
    out << "    \"target\": \"" << json_escape(image_path) << "\"";
    if (!overview_path.empty()) {
        out << ",\n";
        out << "    \"overview\": \"" << json_escape(overview_path) << "\"\n";
    } else {
        out << "\n";
    }
    out << "  },\n";
    out << "  \"metrics\": [\n";
    for (std::size_t i = 0; i < request.metrics.size(); ++i) {
        const DecisionMetric& metric = request.metrics[i];
        out << "    {\"name\": \"" << json_escape(metric.name)
            << "\", \"value\": " << std::setprecision(17) << metric.value
            << ", \"unit\": \"" << json_escape(metric.unit) << "\"}";
        if (i + 1 < request.metrics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"candidates\": [\n";
    for (std::size_t i = 0; i < request.candidates.size(); ++i) {
        const DecisionCandidateView& candidate = request.candidates[i];
        out << "    {\n";
        out << "      \"id\": \"" << json_escape(candidate.id) << "\",\n";
        out << "      \"action\": \"" << json_escape(candidate.action) << "\",\n";
        out << "      \"confidence\": \""
            << json_escape(candidate.confidence) << "\",\n";
        out << "      \"reason\": \"" << json_escape(candidate.reason) << "\"\n";
        out << "    }";
        if (i + 1 < request.candidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

std::string decision_markdown(const DecisionRequest& request,
                              const TargetInfo& target,
                              const std::string& image_path,
                              const std::string& overview_path = {}) {
    std::ostringstream out;
    out << "# " << request.kind << " " << request.target_id << "\n\n";
    out << request.summary << "\n\n";
    out << "- Decision ID: `" << request.id << "`\n";
    out << "- Reason: `" << request.reason << "`\n";
    out << "- Recommended action: `" << request.recommended_action << "`\n";
    if (!target.name.empty()) {
        out << "- Name: `" << target.name << "`\n";
    }
    if (!target.hierarchy_path.empty()) {
        out << "- Hierarchy: `" << target.hierarchy_path << "`\n";
    }
    out << "\n![target](" << image_path << ")\n\n";
    if (!overview_path.empty()) {
        out << "## OCCT Overview\n\n";
        out << "![overview](" << overview_path << ")\n\n";
    }
    out << "## Metrics\n\n";
    if (request.metrics.empty()) {
        out << "No metrics recorded.\n";
    } else {
        for (const DecisionMetric& metric : request.metrics) {
            out << write_metric_line(metric);
        }
    }
    out << "\n## Candidates\n\n";
    for (const DecisionCandidateView& candidate : request.candidates) {
        out << "- `" << candidate.id << "`: `" << candidate.action
            << "` confidence=`" << candidate.confidence << "` - "
            << candidate.reason << "\n";
    }
    return out.str();
}

std::string preview_html(const DecisionRequest& request,
                         const std::string& image_path,
                         const std::string& overview_path = {}) {
    std::ostringstream out;
    out << "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n";
    out << "<title>" << html_escape(request.id) << "</title>\n";
    out << "<style>body{font-family:system-ui,sans-serif;margin:32px;"
        << "color:#1f2933;background:#fafafa} code{background:#eee;padding:2px 4px}"
        << "img{max-width:100%;border:1px solid #ddd;background:white}</style>\n";
    out << "</head><body>\n";
    out << "<h1>" << html_escape(request.kind) << " "
        << html_escape(request.target_id) << "</h1>\n";
    out << "<p>" << html_escape(request.summary) << "</p>\n";
    out << "<p><strong>Recommended action:</strong> <code>"
        << html_escape(request.recommended_action) << "</code></p>\n";
    out << "<img src=\"" << html_escape(image_path) << "\" alt=\"target view\">\n";
    if (!overview_path.empty()) {
        out << "<h2>OCCT overview</h2>\n";
        out << "<img src=\"" << html_escape(overview_path)
            << "\" alt=\"OCCT overview\">\n";
    }
    out << "<h2>Candidates</h2><ul>\n";
    for (const DecisionCandidateView& candidate : request.candidates) {
        out << "<li><code>" << html_escape(candidate.action) << "</code> "
            << html_escape(candidate.confidence) << " - "
            << html_escape(candidate.reason) << "</li>\n";
    }
    out << "</ul>\n";
    out << "</body></html>\n";
    return out.str();
}

DecisionPackResult write_decision_packs_impl(const ConversionReport& report,
                                             const CadModel* model,
                                             const DecisionPackOptions& options) {
    DecisionPackResult result;
    if (options.output_dir.empty()) {
        result.error = "output directory is required";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    if (ec) {
        result.error = "could not create output directory: " + ec.message();
        return result;
    }

    std::size_t considered = 0;
    for (const DecisionRequest& request : report.decision_requests) {
        if (!options.target_id.empty() && request.target_id != options.target_id
            && request.id != options.target_id) {
            continue;
        }
        if (options.max_packets != 0 && considered >= options.max_packets) {
            break;
        }
        ++considered;

        TargetInfo target;
        if (!find_target(report, request.target_id, &target)) {
            target = synthetic_target_for_request(request);
        }

        const std::filesystem::path packet_dir =
            std::filesystem::path(options.output_dir) / slug(request.id);
        std::filesystem::create_directories(packet_dir, ec);
        if (ec) {
            result.error = "could not create packet directory: " + ec.message();
            return result;
        }

        const std::string image_name = "target.svg";
        std::string overview_name;
        std::string error;
        if (model) {
            std::string render_error;
            if (write_occt_overview_png(*model,
                                        packet_dir / "overview.ppm",
                                        &render_error)) {
                overview_name = "overview.ppm";
            } else {
                (void)write_text_file(packet_dir / "occt-render-error.txt",
                                      render_error + "\n",
                                      nullptr);
            }
        }
        if (!write_text_file(packet_dir / image_name,
                             svg_for_request(request, target), &error)
            || !write_text_file(packet_dir / "manifest.json",
                                manifest_json(request,
                                              target,
                                              image_name,
                                              overview_name), &error)
            || !write_text_file(packet_dir / "decision.md",
                                decision_markdown(request,
                                                  target,
                                                  image_name,
                                                  overview_name), &error)
            || !write_text_file(packet_dir / "preview.html",
                                preview_html(request,
                                             image_name,
                                             overview_name), &error)) {
            result.error = error;
            return result;
        }
        ++result.packets_written;
    }

    result.ok = true;
    return result;
}

} // namespace

DecisionPackResult write_decision_packs(const ConversionReport& report,
                                        const DecisionPackOptions& options) {
    return write_decision_packs_impl(report, nullptr, options);
}

DecisionPackResult write_decision_packs(const ConversionReport& report,
                                        const CadModel& model,
                                        const DecisionPackOptions& options) {
    return write_decision_packs_impl(report, &model, options);
}

} // namespace csgify
