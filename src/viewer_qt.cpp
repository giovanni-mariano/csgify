#include "viewer_qt.hpp"

#if CSGIFY_HAVE_QT6_VIEWER

#include "session_view.hpp"
#include "recipe.hpp"
#include "qt/cad_job_controller.hpp"
#include "qt/agent_conversation_dock.hpp"
#if CSGIFY_HAVE_OPENAI_PROVIDER
#include "qt/openai_agent_provider.hpp"
#endif
#include "qt/job_progress_dock.hpp"
#include "qt/view_artifact_capture.hpp"
#include "csgify/project_job_progress.hpp"

#include <AIS_InteractiveContext.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_GridParams.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <Aspect_DisplayConnection.hxx>
#if defined(__APPLE__)
#include <Cocoa_Window.hxx>
#else
#include <Aspect_NeutralWindow.hxx>
#endif
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Graphic3d_GraphicDriverFactory.hxx>
#include <Graphic3d_RenderingParams.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriverFactory.hxx>
#include <Quantity_Color.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <Bnd_Box.hxx>

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QRadioButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

namespace csgify {
namespace {

Quantity_Color status_color(SolidViewStatus status) {
    switch (status) {
    case SolidViewStatus::exact_csg:
        return Quantity_Color(0.15, 0.75, 0.25, Quantity_TOC_sRGB);
    case SolidViewStatus::needs_decomposition:
        return Quantity_Color(0.90, 0.70, 0.10, Quantity_TOC_sRGB);
    case SolidViewStatus::no_interior_point:
        return Quantity_Color(0.85, 0.15, 0.15, Quantity_TOC_sRGB);
    case SolidViewStatus::piece:
        return Quantity_Color(0.20, 0.55, 0.90, Quantity_TOC_sRGB);
    case SolidViewStatus::generated_medium:
        return Quantity_Color(0.05, 0.80, 0.90, Quantity_TOC_sRGB);
    case SolidViewStatus::preview_exact:
        return Quantity_Color(0.10, 0.95, 0.30, Quantity_TOC_sRGB);
    case SolidViewStatus::preview_residual:
        return Quantity_Color(1.00, 0.52, 0.08, Quantity_TOC_sRGB);
    case SolidViewStatus::preview_sliver:
        return Quantity_Color(1.00, 0.10, 0.72, Quantity_TOC_sRGB);
    }
    return Quantity_Color(0.6, 0.6, 0.6, Quantity_TOC_sRGB);
}

QString status_text(SolidViewStatus status) {
    switch (status) {
    case SolidViewStatus::exact_csg: return QStringLiteral("Exact CSG");
    case SolidViewStatus::needs_decomposition: return QStringLiteral("Needs decomposition");
    case SolidViewStatus::no_interior_point: return QStringLiteral("No interior point");
    case SolidViewStatus::piece: return QStringLiteral("Decomposed piece");
    case SolidViewStatus::generated_medium: return QStringLiteral("Contained medium");
    case SolidViewStatus::preview_exact: return QStringLiteral("Preview: exact");
    case SolidViewStatus::preview_residual: return QStringLiteral("Preview: residual");
    case SolidViewStatus::preview_sliver: return QStringLiteral("Preview: sliver");
    }
    return QStringLiteral("Unknown");
}

QString coverage_text(CoverageStatus status) {
    switch (status) {
    case CoverageStatus::certified: return QStringLiteral("certified");
    case CoverageStatus::gap: return QStringLiteral("gap — emission blocked");
    case CoverageStatus::unknown: return QStringLiteral("unknown — emission blocked");
    case CoverageStatus::not_configured: return QStringLiteral("not configured");
    }
    return QStringLiteral("unknown");
}

bool id_matches(const std::string& entry, const std::string& requested) {
    if (entry == requested) return true;
    const auto local = [](const std::string& id) {
        const std::size_t slash = id.rfind('/');
        return slash == std::string::npos ? id : id.substr(slash + 1);
    };
    return local(entry) == local(requested);
}

const DecompositionCandidate* find_candidate(const DecompositionProposal* proposal,
                                             const std::string& id) {
    if (!proposal) return nullptr;
    for (const DecompositionCandidate& candidate : proposal->candidates) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

QString number(double value) {
    return QString::number(value, 'g', 8);
}

ConversionOptions conversionOptionsForProject(
    const AutonomyProjectPolicy& policy) {
    ConversionOptions options;
    options.heal_shape = policy.preparation.heal_shape;
    options.unit_override = policy.preparation.unit_override;
    options.unit_scale_to_mm_override =
        policy.preparation.unit_scale_to_mm_override;
    options.normalize_to_unit = policy.preparation.normalize_to_unit;
    options.approximation = policy.authority.approximation;
    return options;
}

QString authorityModeText(AutonomyMode mode) {
    switch (mode) {
    case AutonomyMode::explain: return QStringLiteral("Advisory");
    case AutonomyMode::propose: return QStringLiteral("Assisted");
    case AutonomyMode::automatic: return QStringLiteral("Delegated");
    }
    return QStringLiteral("Unknown");
}

enum class JobHighlightRole {
    none,
    removal_candidate,
    active_target,
    provisional_processed,
    warning,
};

enum class DisplayState { before, preview, applied };

class OcctViewWidget final : public QWidget {
public:
    explicit OcctViewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(280, 240);
        setAccessibleName(QStringLiteral("3D model viewport"));
        setAccessibleDescription(QStringLiteral(
            "Click to select; drag with the left mouse button to rotate; "
            "drag with the middle button to pan; use the wheel to zoom."));
    }

    QPaintEngine* paintEngine() const override { return nullptr; }

    void setSelectionCallback(std::function<void(const std::string&)> callback) {
        on_selection_ = std::move(callback);
    }

    void setEntries(const std::vector<SolidViewEntry>& entries,
                    const std::string& selected = {}) {
        entries_ = entries;
        pending_selected_ = selected;
        if (initialized_) rebuildScene();
    }

    void setJobHighlights(
        const std::unordered_map<std::string, JobHighlightRole>& highlights) {
        job_highlights_ = highlights;
        if (initialized_) applyPresentation();
    }

    void setActiveJobTarget(const std::string& id) {
        for (auto it = job_highlights_.begin(); it != job_highlights_.end();) {
            if (it->second == JobHighlightRole::active_target)
                it = job_highlights_.erase(it);
            else
                ++it;
        }
        if (!id.empty()) job_highlights_[id] = JobHighlightRole::active_target;
        if (initialized_) applyPresentation();
    }

    void clearJobHighlights() {
        job_highlights_.clear();
        if (initialized_) applyPresentation();
    }

    bool dumpPng(const QString& path) {
        if (view_.IsNull()) return false;
        view_->Redraw();
        Image_PixMap pixels;
        if (!view_->ToPixMap(pixels, width(), height())) return false;
        QImage image(width(), height(), QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const auto color = pixels.PixelColor(x, y).GetRGB();
                row[x] = qRgb(qRound(color.Red() * 255),
                              qRound(color.Green() * 255),
                              qRound(color.Blue() * 255));
            }
        }
        return image.save(path, "PNG");
    }

    void showCandidate(const DecompositionCandidate* candidate) {
        candidate_ = candidate;
        if (initialized_) rebuildOverlays();
    }

    void showReviewOverlays(const ViewerReviewContext& review) {
        review_ = review;
        if (initialized_) rebuildOverlays();
    }

    void fitAll() {
        if (!view_.IsNull()) {
            view_->FitAll(0.02, true);
            view_->Redraw();
        }
    }

    void setGridVisible(bool visible) {
        grid_visible_ = visible;
        updateReferenceAids();
    }

    void setTrihedronVisible(bool visible) {
        trihedron_visible_ = visible;
        updateReferenceAids();
    }

    void showAll() {
        presentation_ = Presentation::opaque_overview;
        applyPresentation();
    }

    void isolate(const std::string& id) {
        selected_id_ = id;
        presentation_ = Presentation::selected_only;
        applyPresentation();
    }

    void showLocalContext(const std::string& id) {
        selected_id_ = id;
        presentation_ = Presentation::local_context;
        applyPresentation();
    }

    void selectId(const std::string& id) {
        if (context_.IsNull()) return;
        selected_id_ = id;
        presentation_ = Presentation::local_context;
        applyPresentation();
        context_->ClearSelected(false);
        for (const auto& item : shapes_) {
            if (id_matches(item.first, id)) {
                context_->SetSelected(item.second, false);
                break;
            }
        }
        context_->UpdateCurrentViewer();
        view_->Redraw();
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (!initialized_) initializeOcct();
    }

    void paintEvent(QPaintEvent*) override {
        if (!view_.IsNull()) view_->Redraw();
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (!window_.IsNull()) {
#if !defined(__APPLE__)
            window_->SetSize(width(), height());
#endif
            view_->MustBeResized();
            view_->Redraw();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        setFocus(Qt::MouseFocusReason);
        press_ = event->position().toPoint();
        last_ = press_;
        dragging_ = false;
        if (!view_.IsNull() && event->button() == Qt::LeftButton) {
            drag_ = Drag::rotate;
            view_->StartRotation(press_.x(), press_.y());
        } else if (event->button() == Qt::MiddleButton) {
            drag_ = Drag::pan;
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (view_.IsNull() || context_.IsNull()) return;
        const QPoint point = event->position().toPoint();
        if (drag_ == Drag::rotate) {
            if ((point - press_).manhattanLength() > 4) dragging_ = true;
            if (dragging_) view_->Rotation(point.x(), point.y());
        } else if (drag_ == Drag::pan) {
            dragging_ = true;
            view_->Pan(point.x() - last_.x(), -(point.y() - last_.y()));
        } else {
            context_->MoveTo(point.x(), point.y(), view_, true);
        }
        last_ = point;
        view_->Redraw();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (view_.IsNull() || context_.IsNull()) return;
        if (event->button() == Qt::LeftButton && !dragging_) {
            const QPoint point = event->position().toPoint();
            context_->MoveTo(point.x(), point.y(), view_, false);
            context_->SelectDetected(AIS_SelectionScheme_Replace);
            for (context_->InitSelected(); context_->MoreSelected();
                 context_->NextSelected()) {
                const Handle(AIS_InteractiveObject) selected =
                    context_->SelectedInteractive();
                const auto found = ais_ids_.find(selected.get());
                if (found != ais_ids_.end()) {
                    selected_id_ = found->second;
                    if (on_selection_) on_selection_(selected_id_);
                    break;
                }
            }
        }
        drag_ = Drag::none;
        dragging_ = false;
        view_->Redraw();
    }

    void wheelEvent(QWheelEvent* event) override {
        if (!view_.IsNull()) {
            view_->SetZoom(event->angleDelta().y() > 0 ? 1.12 : 0.89, true);
            view_->Redraw();
        }
        event->accept();
    }

private:
    enum class Drag { none, rotate, pan };
    enum class Presentation { opaque_overview, local_context, selected_only };

    void initializeOcct() {
        if (Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull()) {
            Graphic3d_GraphicDriverFactory::RegisterFactory(
                new OpenGl_GraphicDriverFactory(), true);
        }
#if defined(__APPLE__)
        // Cocoa defaults to legacy GL unless the core profile is requested.
        const auto factory = Handle(OpenGl_GraphicDriverFactory)::DownCast(
            Graphic3d_GraphicDriverFactory::DefaultDriverFactory());
        if (!factory.IsNull()) factory->DefaultOptions()->contextCompatible = false;
#endif
        display_ = new Aspect_DisplayConnection();
        driver_ = Graphic3d_GraphicDriverFactory::DefaultDriverFactory()
                      ->CreateDriver(display_);
        if (driver_.IsNull()) return;
        viewer_ = new V3d_Viewer(driver_);
        viewer_->SetDefaultLights();
        viewer_->SetLightOn();
        context_ = new AIS_InteractiveContext(viewer_);
        context_->SetDisplayMode(AIS_Shaded, false);
        view_ = viewer_->CreateView();
        view_->ChangeRenderingParams().TransparencyMethod =
            Graphic3d_RTM_DEPTH_PEELING_OIT;
        view_->ChangeRenderingParams().NbOitDepthPeelingLayers = 6;
#if defined(__APPLE__)
        window_ = new Cocoa_Window(reinterpret_cast<NSView*>(winId()));
#else
        window_ = new Aspect_NeutralWindow();
        window_->SetNativeHandle(static_cast<Aspect_Drawable>(winId()));
        window_->SetSize(width(), height());
#endif
        view_->SetWindow(window_);
        view_->SetBgGradientColors(
            Quantity_Color(0.17, 0.17, 0.22, Quantity_TOC_sRGB),
            Quantity_Color(0.09, 0.09, 0.12, Quantity_TOC_sRGB),
            Aspect_GFM_VER, false);
        initialized_ = true;
        updateReferenceAids();
        rebuildScene();
        fitAll();
    }

    static double niceGridStep(double raw_step) {
        if (!std::isfinite(raw_step) || raw_step <= 0.0) return 10.0;
        const double magnitude = std::pow(10.0, std::floor(std::log10(raw_step)));
        const double normalized = raw_step / magnitude;
        const double factor = normalized <= 1.0 ? 1.0
            : normalized <= 2.0 ? 2.0
            : normalized <= 5.0 ? 5.0 : 10.0;
        return factor * magnitude;
    }

    void updateGridGeometry() {
        Bnd_Box scene;
        for (const auto& item : bounds_) scene.Add(item.second);
        double x_span = 100.0;
        double y_span = 100.0;
        double floor_z = 0.0;
        double z_span = 0.0;
        if (!scene.IsVoid()) {
            double xmin, ymin, zmin, xmax, ymax, zmax;
            scene.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            x_span = std::max(std::abs(xmin), std::abs(xmax)) * 2.4;
            y_span = std::max(std::abs(ymin), std::abs(ymax)) * 2.4;
            x_span = std::max(x_span, xmax - xmin);
            y_span = std::max(y_span, ymax - ymin);
            floor_z = zmin;
            z_span = zmax - zmin;
        }
        const double reference_span = std::max({x_span, y_span, 1.0e-6});
        grid_step_ = niceGridStep(reference_span / 20.0);
        grid_size_x_ = std::max(x_span, 20.0 * grid_step_);
        grid_size_y_ = std::max(y_span, 20.0 * grid_step_);
        const double diagonal = std::sqrt(
            x_span * x_span + y_span * y_span + z_span * z_span);
        // A reference floor should never intersect the model. The clearance is
        // tied to both model size and grid spacing so thin or flat parts do not
        // z-fight with it.
        floor_z -= std::max(0.02 * diagonal, 0.5 * grid_step_);
        grid_plane_ = gp_Ax3(gp_Pnt(0.0, 0.0, floor_z),
                             gp_Dir(0.0, 0.0, 1.0),
                             gp_Dir(1.0, 0.0, 0.0));
    }

    void displayReferenceGrid() {
        if (view_.IsNull()) return;
        Aspect_GridParams params;
        params.SetColor(
            Quantity_Color(0.20, 0.22, 0.27, Quantity_TOC_sRGB));
        params.SetAccentColor(
            Quantity_Color(0.29, 0.31, 0.37, Quantity_TOC_sRGB));
        params.SetScale(1.0 / std::max(grid_step_, 1.0e-12));
        params.SetAccentScaleX(1.0 / std::max(10.0 * grid_step_, 1.0e-12));
        params.SetAccentScaleY(1.0 / std::max(10.0 * grid_step_, 1.0e-12));
        params.SetSizeX(grid_size_x_);
        params.SetSizeY(grid_size_y_);
        params.SetLineThickness(0.008);
        params.SetIsDrawAxis(false);
        // Background mode writes maximum depth, so geometry always occludes
        // the reference floor instead of grid lines crossing visible solids.
        params.SetIsBackground(true);
        view_->GridDisplay(params, grid_plane_);
    }

    void updateReferenceAids() {
        if (!initialized_ || viewer_.IsNull() || view_.IsNull()) return;
        updateGridGeometry();
        if (grid_visible_) {
            displayReferenceGrid();
        } else {
            view_->GridErase();
        }
        if (trihedron_visible_) {
            view_->ZBufferTriedronSetup(
                Quantity_Color(0.92, 0.12, 0.10, Quantity_TOC_sRGB),
                Quantity_Color(0.18, 0.82, 0.25, Quantity_TOC_sRGB),
                Quantity_Color(0.15, 0.40, 1.00, Quantity_TOC_sRGB),
                0.80, 0.05, 16);
            view_->TriedronDisplay(Aspect_TOTP_RIGHT_LOWER,
                Quantity_Color(0.92, 0.92, 0.94, Quantity_TOC_sRGB),
                0.085, V3d_ZBUFFER);
        } else {
            view_->TriedronErase();
        }
        view_->Redraw();
    }

    void clearOverlays() {
        if (context_.IsNull()) return;
        for (const Handle(AIS_InteractiveObject)& overlay : overlays_) {
            context_->Remove(overlay, false);
        }
        overlays_.clear();
    }

    void addOverlay(const Handle(AIS_InteractiveObject)& overlay,
                    const Quantity_Color& color,
                    double transparency,
                    int mode = AIS_Shaded) {
        if (overlay.IsNull()) return;
        context_->Display(overlay, mode, -1, false);
        context_->SetColor(overlay, color, false);
        context_->SetTransparency(overlay, transparency, false);
        overlays_.push_back(overlay);
    }

    void rebuildOverlays() {
        if (context_.IsNull()) return;
        clearOverlays();
        if (candidate_) {
            std::vector<PlaneSpec> planes = candidate_->planes;
            if (planes.empty() && candidate_->plane_valid) {
                planes.push_back({candidate_->plane_origin,
                                  candidate_->plane_normal});
            }
            for (const PlaneSpec& spec : planes) {
                try {
                    const gp_Pln plane(
                        gp_Pnt(spec.origin.x, spec.origin.y, spec.origin.z),
                        gp_Dir(spec.normal.x, spec.normal.y, spec.normal.z));
                    BRepBuilderAPI_MakeFace maker(
                        plane, -500.0, 500.0, -500.0, 500.0);
                    if (!maker.IsDone()) continue;
                    addOverlay(new AIS_Shape(maker.Face()),
                        Quantity_Color(0.30, 0.50, 1.00, Quantity_TOC_sRGB),
                        0.55);
                } catch (...) {
                }
            }
        }
        if (review_.has_hole_fill && review_.hole_fill.radius > 0.0
            && review_.hole_fill.length > 0.0) {
            try {
                const HoleFillCandidate& h = review_.hole_fill;
                const gp_Vec height(h.height.x, h.height.y, h.height.z);
                if (height.Magnitude() > 1.0e-12) {
                    BRepPrimAPI_MakeCylinder maker(
                        gp_Ax2(gp_Pnt(h.base.x, h.base.y, h.base.z), gp_Dir(height)),
                        h.radius, h.length);
                    maker.Build();
                    if (maker.IsDone()) {
                        addOverlay(new AIS_Shape(maker.Solid()),
                            Quantity_Color(0.10, 0.85, 0.95, Quantity_TOC_sRGB),
                            0.15);
                    }
                }
            } catch (...) {
            }
        }
        if (review_.has_surface_replacement) {
            const SurfaceReplacementCandidate& s = review_.surface_replacement;
            try {
                Handle(AIS_InteractiveObject) overlay;
                if (s.kind == "plane") {
                    const gp_Pln plane(
                        gp_Pnt(s.origin.x, s.origin.y, s.origin.z),
                        gp_Dir(s.direction.x, s.direction.y, s.direction.z));
                    BRepBuilderAPI_MakeFace maker(
                        plane, -250.0, 250.0, -250.0, 250.0);
                    if (maker.IsDone()) overlay = new AIS_Shape(maker.Face());
                } else if (s.kind == "sphere" && s.radius > 0.0) {
                    overlay = new AIS_Shape(BRepPrimAPI_MakeSphere(
                        gp_Pnt(s.origin.x, s.origin.y, s.origin.z), s.radius).Shape());
                } else if (s.kind == "cylinder" && s.radius > 0.0) {
                    const gp_Dir direction(s.direction.x, s.direction.y,
                                           s.direction.z);
                    const double length = std::max(s.radius * 4.0, 1.0);
                    const gp_Pnt origin(s.origin.x, s.origin.y, s.origin.z);
                    overlay = new AIS_Shape(BRepPrimAPI_MakeCylinder(
                        gp_Ax2(gp_Pnt(origin.XYZ() - direction.XYZ() * (0.5 * length)),
                               direction), s.radius, length).Shape());
                } else if (s.kind == "cone" && s.half_angle > 0.0) {
                    const double length = 4.0;
                    const double radius = std::tan(s.half_angle) * length;
                    overlay = new AIS_Shape(BRepPrimAPI_MakeCone(
                        gp_Ax2(gp_Pnt(s.origin.x, s.origin.y, s.origin.z),
                               gp_Dir(s.direction.x, s.direction.y, s.direction.z)),
                        0.0, radius, length).Shape());
                }
                addOverlay(overlay,
                    Quantity_Color(0.95, 0.35, 0.95, Quantity_TOC_sRGB), 0.45);
            } catch (...) {
            }
        }
        if (review_.has_medium && review_.medium_proposal.ok) {
            const MediumGenerationProposal& medium = review_.medium_proposal;
            try {
                auto add_medium_shape = [&](Vec3 base, Vec3 dimensions,
                                            double radius, double top_radius,
                                            bool conical) {
                    const gp_Vec height(
                        dimensions.x, dimensions.y, dimensions.z);
                    if (height.Magnitude() <= 1.0e-12 || radius <= 0.0) return;
                    const gp_Ax2 axis(
                        gp_Pnt(base.x, base.y, base.z),
                        gp_Dir(height));
                    TopoDS_Shape shape;
                    if (conical && top_radius > 0.0) {
                        shape = BRepPrimAPI_MakeCone(
                            axis, radius, top_radius,
                            height.Magnitude()).Shape();
                    } else {
                        shape = BRepPrimAPI_MakeCylinder(
                            axis, radius, height.Magnitude()).Shape();
                    }
                    if (!shape.IsNull()) {
                        addOverlay(new AIS_Shape(shape),
                            Quantity_Color(0.10, 0.85, 0.95,
                                           Quantity_TOC_sRGB), 0.18);
                    }
                };
                if (!medium.network.segments.empty()) {
                    for (const MediumNetworkSegment& segment
                         : medium.network.segments) {
                        add_medium_shape(
                            {segment.base[0], segment.base[1], segment.base[2]},
                            {segment.height[0], segment.height[1],
                             segment.height[2]},
                            segment.radius, segment.top_radius,
                            segment.conical);
                    }
                } else {
                    add_medium_shape(medium.base, medium.height, medium.radius,
                                     medium.top_radius, medium.conical);
                }
            } catch (...) {
            }
        }
        context_->UpdateCurrentViewer();
        view_->Redraw();
    }

    void rebuildScene() {
        if (context_.IsNull()) return;
        clearOverlays();
        for (const auto& item : shapes_) context_->Remove(item.second, false);
        shapes_.clear();
        ais_ids_.clear();
        source_ids_.clear();
        statuses_.clear();
        bounds_.clear();
        overlap_ids_.clear();
        for (const SolidViewEntry& entry : entries_) {
            if (entry.shape.IsNull()) continue;
            Handle(AIS_Shape) shape = new AIS_Shape(entry.shape);
            context_->Display(shape, AIS_Shaded, 0, false);
            context_->SetColor(shape, status_color(entry.status), false);
            context_->SetTransparency(shape, 0.0, false);
            shapes_.push_back({entry.shape_id, shape});
            ais_ids_[shape.get()] = entry.shape_id;
            source_ids_[entry.shape_id] = entry.source_solid_id;
            statuses_[entry.shape_id] = entry.status;
            if (entry.overlap_conflict) overlap_ids_.insert(entry.shape_id);
            Bnd_Box bounds;
            BRepBndLib::Add(entry.shape, bounds);
            bounds_.emplace(entry.shape_id, bounds);
        }
        updateGridGeometry();
        if (grid_visible_) displayReferenceGrid();
        rebuildOverlays();
        if (!pending_selected_.empty()) {
            selectId(pending_selected_);
        } else {
            presentation_ = Presentation::opaque_overview;
            applyPresentation();
        }
        context_->UpdateCurrentViewer();
        view_->Redraw();
    }

    bool isSelectedShape(const std::string& id) const {
        if (id_matches(id, selected_id_)) return true;
        const auto source = source_ids_.find(id);
        return source != source_ids_.end() && !source->second.empty()
            && id_matches(source->second, selected_id_);
    }

    Quantity_Color normalColor(const std::string& id) const {
        if (overlap_ids_.count(id))
            return Quantity_Color(0.95, 0.12, 0.08, Quantity_TOC_sRGB);
        const auto status = statuses_.find(id);
        if (status != statuses_.end()) return status_color(status->second);
        return Quantity_Color(0.55, 0.58, 0.62, Quantity_TOC_sRGB);
    }

    JobHighlightRole jobRole(const std::string& id) const {
        for (const auto& item : job_highlights_) {
            if (id_matches(id, item.first)) return item.second;
            const auto source = source_ids_.find(id);
            if (source != source_ids_.end() && id_matches(source->second, item.first))
                return item.second;
        }
        return JobHighlightRole::none;
    }

    void applyPresentation() {
        if (context_.IsNull()) return;
        Bnd_Box focus;
        if (!selected_id_.empty()) {
            for (const auto& item : bounds_) {
                if (isSelectedShape(item.first)) focus.Add(item.second);
            }
        }
        if (!focus.IsVoid()) {
            double xmin, ymin, zmin, xmax, ymax, zmax;
            focus.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            const double diagonal = std::sqrt(
                (xmax - xmin) * (xmax - xmin)
                + (ymax - ymin) * (ymax - ymin)
                + (zmax - zmin) * (zmax - zmin));
            focus.Enlarge(std::max(1.0e-6, 0.10 * diagonal));
        }
        for (const auto& item : shapes_) {
            const bool selected = isSelectedShape(item.first);
            bool visible = true;
            bool context_shape = false;
            if (presentation_ == Presentation::selected_only) {
                visible = selected;
            } else if (presentation_ == Presentation::local_context
                       && !focus.IsVoid()) {
                const auto bounds = bounds_.find(item.first);
                context_shape = !selected && !focus.IsVoid()
                    && bounds != bounds_.end() && !focus.IsOut(bounds->second);
                visible = selected || context_shape;
            }
            if (!visible) {
                context_->Erase(item.second, false);
                continue;
            }
            context_->Display(item.second, AIS_Shaded, 0, false);
            if (selected) {
                context_->SetColor(item.second,
                    Quantity_Color(0.05, 0.78, 1.00, Quantity_TOC_sRGB), false);
                context_->SetTransparency(item.second, 0.0, false);
            } else if (overlap_ids_.count(item.first)) {
                context_->SetColor(item.second,
                    Quantity_Color(0.98, 0.05, 0.02, Quantity_TOC_sRGB), false);
                context_->SetTransparency(item.second, 0.0, false);
            } else if (jobRole(item.first) == JobHighlightRole::active_target) {
                context_->SetColor(item.second,
                    Quantity_Color(1.00, 0.88, 0.05, Quantity_TOC_sRGB), false);
                context_->SetTransparency(item.second, 0.0, false);
            } else if (jobRole(item.first) == JobHighlightRole::warning) {
                context_->SetColor(item.second,
                    Quantity_Color(1.00, 0.45, 0.05, Quantity_TOC_sRGB), false);
                context_->SetTransparency(item.second, 0.05, false);
            } else if (jobRole(item.first) == JobHighlightRole::removal_candidate) {
                context_->SetColor(item.second,
                    Quantity_Color(0.92, 0.10, 0.08, Quantity_TOC_sRGB), false);
                context_->SetTransparency(item.second, 0.10, false);
            } else if (jobRole(item.first) == JobHighlightRole::provisional_processed) {
                context_->SetColor(item.second,
                    Quantity_Color(0.42, 0.45, 0.50, Quantity_TOC_sRGB), false);
                context_->SetTransparency(item.second, 0.65, false);
            } else if (context_shape) {
                context_->SetColor(item.second,
                    overlap_ids_.count(item.first)
                        ? Quantity_Color(0.95, 0.12, 0.08, Quantity_TOC_sRGB)
                        : Quantity_Color(0.48, 0.51, 0.56, Quantity_TOC_sRGB),
                    false);
                context_->SetTransparency(item.second,
                    overlap_ids_.count(item.first) ? 0.45 : 0.72, false);
            } else {
                context_->SetColor(item.second, normalColor(item.first), false);
                context_->SetTransparency(item.second, 0.0, false);
            }
        }
        context_->UpdateCurrentViewer();
        if (!view_.IsNull()) view_->Redraw();
    }

    bool initialized_ = false;
    bool grid_visible_ = true;
    bool trihedron_visible_ = true;
    double grid_step_ = 10.0;
    double grid_size_x_ = 200.0;
    double grid_size_y_ = 200.0;
    gp_Ax3 grid_plane_ = gp_Ax3(gp_Pnt(0.0, 0.0, -5.0),
                                gp_Dir(0.0, 0.0, 1.0),
                                gp_Dir(1.0, 0.0, 0.0));
    Handle(Aspect_DisplayConnection) display_;
    Handle(Graphic3d_GraphicDriver) driver_;
    Handle(V3d_Viewer) viewer_;
    Handle(AIS_InteractiveContext) context_;
    Handle(V3d_View) view_;
#if defined(__APPLE__)
    Handle(Cocoa_Window) window_;
#else
    Handle(Aspect_NeutralWindow) window_;
#endif
    std::vector<SolidViewEntry> entries_;
    std::vector<std::pair<std::string, Handle(AIS_Shape)>> shapes_;
    std::map<AIS_InteractiveObject*, std::string> ais_ids_;
    std::map<std::string, std::string> source_ids_;
    std::map<std::string, SolidViewStatus> statuses_;
    std::map<std::string, Bnd_Box> bounds_;
    std::set<std::string> overlap_ids_;
    std::unordered_map<std::string, JobHighlightRole> job_highlights_;
    std::vector<Handle(AIS_InteractiveObject)> overlays_;
    const DecompositionCandidate* candidate_ = nullptr;
    ViewerReviewContext review_;
    std::string selected_id_;
    std::string pending_selected_;
    std::function<void(const std::string&)> on_selection_;
    Drag drag_ = Drag::none;
    Presentation presentation_ = Presentation::opaque_overview;
    QPoint press_;
    QPoint last_;
    bool dragging_ = false;
};

struct QtRemovalPlanEntry {
    std::string component_id;
    std::string definition_id;
    std::string label;
    std::string confidence;
    std::string suggested_action;
    double volume_fraction = 0.0;
    double score = 0.0;
    std::vector<std::string> target_ids;
    std::vector<std::string> review_ids;
    std::vector<std::string> reason_codes;
    std::vector<std::string> blockers;
    bool protected_target = false;
    bool auto_eligible = false;
    bool previously_rejected = false;
};

class DecisionWindow final : public QMainWindow {
public:
    DecisionWindow(Session& session,
                   std::string step_path,
                   std::string document_id,
                   std::string preselect_id,
                   const DecompositionProposal* proposal,
                   std::string candidate_id,
                   ViewerReviewContext review,
                   std::string commit_path,
                   std::function<void(bool, const std::string&, bool)>
                       decision_callback = {},
                   bool focused_initial_display = false,
                   ProjectStore* project = nullptr)
        : session_(session),
          step_path_(std::move(step_path)),
          document_id_(std::move(document_id)),
          proposal_(proposal),
          commit_path_(std::move(commit_path)),
          controller_(session_, document_id_, std::move(review)),
          selected_id_(std::move(preselect_id)),
          decision_callback_(std::move(decision_callback)),
          project_(project),
          focused_initial_display_(focused_initial_display) {
        initializeRemovalPlan();
        checkpointPendingReviews();
        setWindowTitle(QStringLiteral("csgify geometry workspace — ")
                       + QString::fromStdString(step_path_));
        resize(1500, 900);
        setObjectName(QStringLiteral("geometryWorkspace"));
        QFont workspace_font(QStringLiteral("Noto Sans"));
        workspace_font.setPointSize(10);
        setFont(workspace_font);
        setStyleSheet(QStringLiteral(
            "QMainWindow#geometryWorkspace { background: #edf2f5; }"
            "QWidget#workspaceBody, QWidget#assemblyPanel, QWidget#viewportPanel,"
            "QWidget#decisionPanel { background: #ffffff; color: #203442; }"
            "QLabel { color: #203442; }"
            "QLabel[role='secondary'] { color: #526775; }"
            "QLabel[role='heading'] { font-weight: 600; font-size: 13px; }"
            "QPushButton { background: #f5f8fa; color: #203442; border: 1px solid #bccbd4;"
            " border-radius: 4px; padding: 6px 9px; }"
            "QPushButton:hover { background: #e7f1f5; border-color: #126b80; }"
            "QPushButton:checked { background: #d8f3f7; border-color: #126b80; }"
            "QPushButton[role='primary'] { background: #126b80; color: #ffffff; border-color: #126b80; }"
            "QPushButton[role='primary']:hover { background: #0c5769; }"
            "QPushButton:disabled { color: #73838d; background: #edf2f5; border-color: #d6dfe5; }"
            "QPushButton:focus, QLineEdit:focus { border: 2px solid #126b80; }"
            "QLineEdit { background: #ffffff; color: #203442; border: 1px solid #bccbd4;"
            " border-radius: 4px; padding: 7px; selection-background-color: #126b80; }"
            "QTreeWidget, QTableWidget, QTextBrowser, QPlainTextEdit { background: #ffffff;"
            " color: #203442; border: 1px solid #d6dfe5; selection-background-color: #d8f3f7;"
            " selection-color: #203442; }"
            "QTreeWidget::item, QTableWidget::item { padding: 5px; }"
            "QHeaderView::section { background: #edf2f5; color: #526775; border: none;"
            " border-bottom: 1px solid #d6dfe5; padding: 7px; }"
            "QTabWidget::pane { border: 1px solid #d6dfe5; background: #ffffff; }"
            "QTabBar::tab { background: #edf2f5; color: #526775; padding: 8px 12px; }"
            "QTabBar::tab:selected { background: #ffffff; color: #126b80; }"
            "QRadioButton { color: #203442; padding: 4px; }"
            "QSplitter::handle { background: #edf2f5; }"
            "QProgressBar { background: #edf2f5; color: #203442; border: 1px solid #d6dfe5;"
            " border-radius: 3px; text-align: center; min-height: 18px; }"
            "QProgressBar::chunk { background: #8cc9d5; }"
            "QDockWidget { color: #203442; }"));

        auto* body = new QWidget(this);
        body->setObjectName(QStringLiteral("workspaceBody"));
        auto* body_layout = new QVBoxLayout(body);
        body_layout->setContentsMargins(12, 12, 12, 8);
        body_layout->setSpacing(12);
        auto* header = new QHBoxLayout;
        auto* identity = new QVBoxLayout;
        auto* filename = new QLabel(QString::fromStdString(
            std::filesystem::path(step_path_).filename().string()), body);
        filename->setObjectName(QStringLiteral("workspaceModelName"));
        filename->setToolTip(QString::fromStdString(step_path_));
        QFont filename_font = workspace_font;
        filename_font.setPointSize(14);
        filename_font.setBold(true);
        filename->setFont(filename_font);
        filename->setWordWrap(true);
        filename->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        identity->addWidget(filename);
        workspace_status_ = new QLabel(body);
        workspace_status_->setObjectName(QStringLiteral("workspaceGeometryStatus"));
        workspace_status_->setProperty("role", "secondary");
        workspace_status_->setWordWrap(true);
        identity->addWidget(workspace_status_);
        header->addLayout(identity, 1);
        auto* history_button = new QPushButton(QStringLiteral("History"), body);
        history_button->setObjectName(QStringLiteral("workspaceHistoryButton"));
        history_button->setCheckable(true);
        header->addWidget(history_button);
        body_layout->addLayout(header);

        auto* splitter = new QSplitter(Qt::Horizontal, body);
        splitter->setObjectName(QStringLiteral("geometryWorkspaceSplitter"));
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(6);
        auto* assembly = new QWidget(splitter);
        assembly->setObjectName(QStringLiteral("assemblyPanel"));
        assembly->setMinimumWidth(210);
        auto* assembly_layout = new QVBoxLayout(assembly);
        assembly_layout->setContentsMargins(0, 0, 0, 0);
        auto* assembly_title = new QLabel(QStringLiteral("Assembly"), assembly);
        assembly_title->setProperty("role", "heading");
        assembly_layout->addWidget(assembly_title);
        model_search_ = new QLineEdit(assembly);
        model_search_->setObjectName(QStringLiteral("modelSearchField"));
        model_search_->setPlaceholderText(QStringLiteral("Search name, ID or status"));
        model_search_->setAccessibleName(QStringLiteral("Search model hierarchy"));
        model_search_->setClearButtonEnabled(true);
        assembly_layout->addWidget(model_search_);
        tree_ = new QTreeWidget(assembly);
        tree_->setObjectName(QStringLiteral("modelStatusTree"));
        tree_->setHeaderLabels({QStringLiteral("Shape"), QStringLiteral("Status")});
        tree_->setMinimumWidth(0);
        tree_->setUniformRowHeights(true);
        tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree_->setAccessibleName(QStringLiteral("Model hierarchy and status"));
        assembly_layout->addWidget(tree_, 1);
        search_status_ = new QLabel(assembly);
        search_status_->setProperty("role", "secondary");
        search_status_->setWordWrap(true);
        assembly_layout->addWidget(search_status_);
        connect(model_search_, &QLineEdit::textChanged, this,
                [this]() { filterModelTree(); });

        auto* viewport_panel = new QWidget(splitter);
        viewport_panel->setObjectName(QStringLiteral("viewportPanel"));
        viewport_layout_ = new QVBoxLayout(viewport_panel);
        viewport_layout_->setContentsMargins(0, 0, 0, 0);
        auto* viewport_title = new QLabel(QStringLiteral("Geometry"), viewport_panel);
        viewport_title->setProperty("role", "heading");
        viewport_layout_->addWidget(viewport_title);
        viewport_ = new OcctViewWidget(viewport_panel);
        viewport_->setObjectName(QStringLiteral("nativeGeometryViewport"));
        viewport_layout_->addWidget(viewport_, 1);
        auto* inspector_scroll = new QScrollArea(splitter);
        inspector_scroll->setObjectName(QStringLiteral("geometryInspectorScroll"));
        inspector_scroll->setWidgetResizable(true);
        inspector_scroll->setFrameShape(QFrame::NoFrame);
        inspector_scroll->setMinimumWidth(320);
        inspector_scroll->setWidget(buildDecisionPanel(inspector_scroll));
        splitter->addWidget(assembly);
        splitter->addWidget(viewport_panel);
        splitter->addWidget(inspector_scroll);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setStretchFactor(2, 0);
        splitter->setSizes({270, 750, 420});
        body_layout->addWidget(splitter, 1);
        setCentralWidget(body);

        auto* diagnostics_dock = new QDockWidget(QStringLiteral("Decision history"), this);
        history_ = new QPlainTextEdit(diagnostics_dock);
        history_->setReadOnly(true);
        history_->setMaximumBlockCount(1000);
        diagnostics_dock->setWidget(history_);
        addDockWidget(Qt::BottomDockWidgetArea, diagnostics_dock);
        diagnostics_dock->hide();
        connect(history_button, &QPushButton::toggled,
                diagnostics_dock, &QDockWidget::setVisible);
        connect(diagnostics_dock, &QDockWidget::visibilityChanged,
                history_button, &QPushButton::setChecked);

        job_controller_ = new CadJobController(this);
        job_dock_ = new JobProgressDock(this);
        progress_visual_timer_ = new QTimer(this);
        progress_visual_timer_->setSingleShot(true);
        progress_visual_timer_->setInterval(100);
        connect(progress_visual_timer_, &QTimer::timeout, this,
                [this]() { flushProgressVisual(); });
        addDockWidget(Qt::BottomDockWidgetArea, job_dock_);
        tabifyDockWidget(diagnostics_dock, job_dock_);
        job_dock_->hide();
        connect(job_controller_, &CadJobController::progress, this,
                [this](const JobProgressEvent& event) { updateJobProgress(event); });
        connect(job_controller_, &CadJobController::fixedPointCompleted, this,
                [this](const FixedPointResultPtr& result) {
                    if (result) fixedPointFinished(*result);
                });
        connect(job_controller_, &CadJobController::previewCompleted, this,
                [this](const DecompositionPreviewPtr& result) {
                    if (result) previewFinished(*result);
                });
        connect(job_controller_, &CadJobController::commandCompleted, this,
                [this](const CommandResultPtr& result) {
                    if (!result) return;
                    if (removal_running_) removalFinished(*result);
                    else if (emission_running_) emissionFinished(*result);
                });
        connect(job_controller_, &CadJobController::failed, this,
                [this](const QString& code, const QString& message) {
                    appendHistory(code.toStdString() + ": " + message.toStdString());
                    if (emission_running_) {
                        emission_running_ = false;
                        close_after_emission_ = false;
                        restoreAfterEmission();
                        QMessageBox::critical(this, QStringLiteral("Emission failed"),
                                              message);
                        return;
                    }
                    failProjectRemoval(code.toStdString(), message.toStdString());
                    QMessageBox::critical(this, QStringLiteral("Geometry job failed"), message);
                });
        connect(job_controller_, &CadJobController::activeChanged, this,
                [this](bool active) {
                    if (active) return;
                    if (fixed_point_running_) {
                        fixed_point_running_ = false;
                        cancel_fixed_point_button_->setEnabled(false);
                        run_fixed_point_button_->setEnabled(true);
                        undo_button_->setEnabled(true);
                        before_button_->setEnabled(true);
                        applied_button_->setEnabled(true);
                        review_residual_button_->setEnabled(true);
                    }
                    if (preview_running_) {
                        preview_running_ = false;
                        cancel_fixed_point_button_->setEnabled(false);
                        run_fixed_point_button_->setEnabled(true);
                        accept_button_->setEnabled(true);
                        reject_button_->setEnabled(true);
                        undo_button_->setEnabled(true);
                        candidates_->setEnabled(true);
                        before_button_->setEnabled(true);
                        applied_button_->setEnabled(true);
                        const DecompositionCandidate* candidate = selectedCandidate();
                        preview_button_->setEnabled(candidate && candidate->usable);
                        review_residual_button_->setEnabled(true);
                    }
                    if (removal_running_) {
                        removal_running_ = false;
                        clear_decision_audit_context(session_);
                        cancel_fixed_point_button_->setEnabled(false);
                        run_fixed_point_button_->setEnabled(
                            !publication_recovery_required_);
                        accept_button_->setEnabled(
                            !removal_completed_success_
                            && !publication_recovery_required_);
                        reject_button_->setEnabled(
                            !removal_completed_success_
                            && !publication_recovery_required_);
                        undo_button_->setEnabled(true);
                        candidates_->setEnabled(
                            !removal_completed_success_
                            && !publication_recovery_required_);
                        select_removals_button_->setEnabled(
                            !removal_completed_success_
                            && !publication_recovery_required_);
                        before_button_->setEnabled(true);
                        preview_button_->setEnabled(false);
                        applied_button_->setEnabled(true);
                        review_residual_button_->setEnabled(
                            !publication_recovery_required_);
                        if (removal_close_after_finish_) {
                            removal_close_after_finish_ = false;
                            if (!publication_recovery_required_
                                && decision_callback_) {
                                decision_callback_(removal_completed_success_, {}, false);
                            }
                            if (!publication_recovery_required_)
                                QTimer::singleShot(0, this, &QWidget::close);
                        }
                    }
                    if (!active && close_after_emission_ && committed_) {
                        close_after_emission_ = false;
                        QTimer::singleShot(0, this, &QWidget::close);
                    }
                    updateDecompositionActionState();
                });
        connect(job_dock_, &JobProgressDock::cancelRequested, this, [this]() {
            requestProjectRemovalCancellation();
            job_controller_->cancel();
        });
        connect(job_dock_, &JobProgressDock::showCurrentRequested, this, [this] {
            if (!active_job_target_.empty()) viewport_->selectId(active_job_target_);
        });
        connect(job_dock_, &JobProgressDock::showPlanRequested, this, [this] {
            setDisplayState(DisplayState::before);
            viewport_->setJobHighlights(job_highlights_);
        });
        connect(job_dock_, &JobProgressDock::fitRequested,
                viewport_, &OcctViewWidget::fitAll);
        connect(job_dock_, &JobProgressDock::openArtifactRequested, this,
                [](const QString& path) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                });

        viewport_->setSelectionCallback([this](const std::string& id) {
            selected_id_ = id;
            selectTreeId(id);
            appendHistory("selected " + id);
            selectionChanged();
        });
        connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this]() {
            const auto selected = tree_->selectedItems();
            if (selected.empty()) return;
            selected_id_ = selected.front()->data(0, Qt::UserRole)
                               .toString().toStdString();
            viewport_->selectId(selected_id_);
            selectionChanged();
        });

        refreshGeometry();
        populateCandidates(candidate_id);
        viewport_->showReviewOverlays(controller_.review());
        updateDetails();
        configureDecisionControls();

        new QShortcut(QKeySequence(Qt::Key_F), this, [this]() { viewport_->fitAll(); });
        accept_shortcut_ = new QShortcut(
            QKeySequence(Qt::CTRL | Qt::Key_Return), this,
            [this]() { accept(); });
        accept_shortcut_->setObjectName(QStringLiteral("acceptProposalShortcut"));
        accept_shortcut_->setEnabled(!controller_.review().kind.empty()
                                     && !removalReviewMode());
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), this, [this]() { undo(); });
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if ((job_controller_ && job_controller_->active())
            || residual_review_running_) {
            if (job_controller_) job_controller_->cancel();
            appendHistory("a geometry task is still finishing; close again after it stops");
            event->ignore();
            return;
        }
        if (publication_recovery_required_) {
            QMessageBox::critical(this,
                QStringLiteral("Recovery required before closing"),
                QStringLiteral("The live geometry and durable project state do "
                               "not agree. Repair project storage and choose "
                               "Retry recovery before closing this workspace."));
            event->ignore();
            return;
        }
        if (!commit()) {
            event->ignore();
            return;
        }
        event->accept();
    }

private:
    bool removalReviewMode() const {
        const std::string& kind = controller_.review().kind;
        return kind == "removal" || kind == "removal_set";
    }

    void checkpointPendingReviews() {
        const ViewerReviewContext& review = controller_.review();
        const std::size_t revision = document_revision(session_, document_id_);
        const auto checkpoint = [&](const std::string& target,
                                    const std::string& proposal) {
            if (target.empty() || proposal.empty()
                || !decision_review_status(session_, target, proposal).empty()) {
                return;
            }
            DecisionAuditContext audit = review.audit;
            if (audit.actor.empty()) audit.actor = "reviewer";
            if (audit.proposal_id.empty()) audit.proposal_id = proposal;
            record_decision_review(session_, target, proposal, "pending", audit,
                review.kind, review.action, review.candidate_id, revision);
        };
        if (review.kind == "removal_set") {
            for (const ViewerReviewAlternative& alternative
                 : review.alternatives) {
                checkpoint(alternative.target_id, alternative.review_id);
            }
        } else {
            checkpoint(review.target_id, review.review_id);
        }
    }

    void recordActiveReviewOutcome(const std::string& decision) {
        const ViewerReviewContext& review = controller_.review();
        if (review.target_id.empty() || review.review_id.empty()) return;
        DecisionAuditContext audit = review.audit;
        if (audit.actor.empty()) audit.actor = "reviewer";
        if (audit.proposal_id.empty()) audit.proposal_id = review.review_id;
        record_decision_review(session_, review.target_id, review.review_id,
            decision, audit, review.kind, review.action, review.candidate_id,
            document_revision(session_, document_id_));
    }

    void initializeRemovalPlan() {
        if (!removalReviewMode()) return;
        removal_base_revision_ = document_revision(session_, document_id_);
        std::vector<ViewerReviewAlternative> alternatives;
        for (const ControllerAlternative& item : controller_.alternatives())
            alternatives.push_back(item.info);
        if (alternatives.empty() && !controller_.review().target_id.empty()) {
            ViewerReviewAlternative item;
            item.review_id = controller_.review().review_id;
            item.target_id = controller_.review().target_id;
            item.label = item.target_id;
            item.confidence = "manual";
            item.suggested_action = "review_remove_solid";
            const ConversionReport& report = last_report(session_);
            if (report.document_id.empty()
                || id_matches(report.document_id, document_id_)) {
                const auto solid = std::find_if(report.solids.begin(),
                    report.solids.end(), [&](const AnalyzedSolid& value) {
                        return id_matches(value.id, item.target_id);
                    });
                if (solid != report.solids.end()) {
                    item.label = solid->label;
                    item.name = solid->name;
                    item.hierarchy_path = solid->hierarchy_path;
                    item.volume_fraction = solid->volume_fraction;
                    const auto instance = std::find_if(
                        report.component_instances.begin(),
                        report.component_instances.end(),
                        [&](const ComponentInstance& value) {
                            return id_matches(value.source_shape_id,
                                              item.target_id);
                        });
                    if (instance != report.component_instances.end()) {
                        item.component_instance_id = instance->id;
                        item.component_definition_id = instance->definition_id;
                    } else if (!solid->hierarchy_path.empty()) {
                        const auto node = std::find_if(report.assembly.begin(),
                            report.assembly.end(), [&](const AssemblyNode& value) {
                                return value.kind == "shape"
                                    && value.path == solid->hierarchy_path;
                            });
                        if (node != report.assembly.end()) {
                            item.component_instance_id = node->instance_id;
                            item.component_definition_id = node->definition_id;
                        }
                    }
                }
            }
            alternatives.push_back(std::move(item));
        }

        std::map<std::string, std::size_t> by_component;
        const auto append_unique = [](std::vector<std::string>* values,
                                      const std::string& value) {
            if (!value.empty()
                && std::find(values->begin(), values->end(), value)
                       == values->end()) {
                values->push_back(value);
            }
        };
        const auto confidence_rank = [](const std::string& confidence) {
            if (confidence == "high") return 3;
            if (confidence == "medium") return 2;
            if (confidence == "low") return 1;
            return 0;
        };
        for (const ViewerReviewAlternative& item : alternatives) {
            const std::string key = item.component_instance_id.empty()
                ? "solid-only:" + item.target_id : item.component_instance_id;
            auto found = by_component.find(key);
            if (found == by_component.end()) {
                QtRemovalPlanEntry entry;
                entry.component_id = item.component_instance_id;
                entry.definition_id = item.component_definition_id;
                entry.label = !item.name.empty() ? item.name
                    : (!item.hierarchy_path.empty() ? item.hierarchy_path
                                                    : item.label);
                entry.confidence = item.confidence;
                entry.suggested_action = item.suggested_action;
                entry.score = item.score;
                found = by_component.emplace(key, removal_plan_.size()).first;
                removal_plan_.push_back(std::move(entry));
            }
            QtRemovalPlanEntry& entry = removal_plan_[found->second];
            if (!item.review_id.empty()
                && decision_review_status(session_, item.target_id,
                    item.review_id) == "rejected") {
                entry.previously_rejected = true;
            }
            const bool new_target = std::find(entry.target_ids.begin(),
                entry.target_ids.end(), item.target_id) == entry.target_ids.end();
            append_unique(&entry.target_ids, item.target_id);
            if (new_target) entry.review_ids.push_back(item.review_id);
            for (const std::string& reason : item.reason_codes)
                append_unique(&entry.reason_codes, reason);
            for (const std::string& blocker : item.blockers)
                append_unique(&entry.blockers, blocker);
            entry.volume_fraction += item.volume_fraction;
            entry.score = std::max(entry.score, item.score);
            if (confidence_rank(item.confidence)
                < confidence_rank(entry.confidence)) {
                entry.confidence = item.confidence;
            }
            if (item.suggested_action == "manual")
                entry.suggested_action = "manual";
        }
        for (QtRemovalPlanEntry& entry : removal_plan_) {
            entry.protected_target = std::find(
                entry.blockers.begin(), entry.blockers.end(),
                "protected_name_or_material") != entry.blockers.end();
            entry.auto_eligible = !entry.protected_target
                && !entry.previously_rejected
                && entry.blockers.empty() && entry.confidence == "high"
                && entry.suggested_action == "remove_solid";
        }
    }

    static QString joined(const std::vector<std::string>& values) {
        QStringList result;
        for (const std::string& value : values)
            result.push_back(QString::fromStdString(value));
        return result.join(QStringLiteral(", "));
    }

    QWidget* buildDecisionPanel(QWidget* parent) {
        auto* panel = new QWidget(parent);
        panel->setObjectName(QStringLiteral("decisionPanel"));
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 0, 8, 8);
        layout->setSpacing(12);
        title_ = new QLabel(QStringLiteral("Inspector"), panel);
        QFont title_font = title_->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 2);
        title_->setFont(title_font);
        layout->addWidget(title_);

        summary_ = new QLabel(panel);
        summary_->setObjectName(QStringLiteral("decisionSummary"));
        summary_->setWordWrap(true);
        summary_->setTextFormat(Qt::PlainText);
        layout->addWidget(summary_);

        auto* mode_row = new QHBoxLayout();
        before_button_ = new QRadioButton(QStringLiteral("Before"), panel);
        preview_button_ = new QRadioButton(QStringLiteral("Preview"), panel);
        applied_button_ = new QRadioButton(QStringLiteral("Verified result"), panel);
        before_button_->setObjectName(QStringLiteral("beforeGeometryButton"));
        preview_button_->setObjectName(QStringLiteral("previewGeometryButton"));
        applied_button_->setObjectName(QStringLiteral("verifiedGeometryButton"));
        auto* mode_group = new QButtonGroup(panel);
        mode_group->addButton(before_button_);
        mode_group->addButton(preview_button_);
        mode_group->addButton(applied_button_);
        before_button_->setChecked(true);
        mode_row->addWidget(before_button_);
        mode_row->addWidget(preview_button_);
        mode_row->addWidget(applied_button_);
        viewport_layout_->insertLayout(1, mode_row);

        inspector_tabs_ = new QTabWidget(panel);
        inspector_tabs_->setObjectName(QStringLiteral("geometryInspectorTabs"));
        inspector_tabs_->setMinimumHeight(280);
        auto* alternatives = new QWidget(inspector_tabs_);
        auto* alternatives_layout = new QVBoxLayout(alternatives);
        alternatives_layout->setContentsMargins(0, 0, 0, 0);

        candidates_ = new QTableWidget(panel);
        candidates_->setObjectName(QStringLiteral("proposalCandidatesTable"));
        candidates_->setColumnCount(6);
        if (removalReviewMode()) {
            candidates_->setHorizontalHeaderLabels({QStringLiteral("Remove"),
                QStringLiteral("Component"), QStringLiteral("Solids"),
                QStringLiteral("Confidence"), QStringLiteral("Volume %"),
                QStringLiteral("Status")});
        } else {
            candidates_->setHorizontalHeaderLabels({QStringLiteral("Candidate"),
                QStringLiteral("Confidence"), QStringLiteral("Pieces"),
                QStringLiteral("Exact"), QStringLiteral("Residual"),
                QStringLiteral("Δ volume")});
        }
        candidates_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        candidates_->horizontalHeader()->setStretchLastSection(true);
        candidates_->setSelectionBehavior(QAbstractItemView::SelectRows);
        candidates_->setSelectionMode(QAbstractItemView::SingleSelection);
        candidates_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        candidates_->verticalHeader()->hide();
        candidates_->setShowGrid(false);
        candidates_->setAccessibleName(removalReviewMode()
            ? QStringLiteral("Component removal plan")
            : QStringLiteral("Geometry proposal candidates"));
        alternatives_layout->addWidget(candidates_, 1);
        select_removals_button_ = new QPushButton(
            QStringLiteral("Select eligible high-confidence"), panel);
        select_removals_button_->setVisible(removalReviewMode());
        alternatives_layout->addWidget(select_removals_button_);
        inspector_tabs_->addTab(alternatives, QStringLiteral("Alternatives"));

        details_ = new QTextBrowser(panel);
        details_->setObjectName(QStringLiteral("geometryEvidenceBrowser"));
        details_->setMinimumHeight(200);
        inspector_tabs_->addTab(details_, QStringLiteral("Evidence"));
        layout->addWidget(inspector_tabs_, 1);

        auto* first_row = new QHBoxLayout();
        accept_button_ = new QPushButton(QStringLiteral("Accept"), panel);
        accept_button_->setObjectName(QStringLiteral("acceptProposalButton"));
        accept_button_->setProperty("role", "primary");
        accept_button_->setToolTip(QStringLiteral(
            "Apply the current proposal (Ctrl+Enter)"));
        reject_button_ = new QPushButton(QStringLiteral("Reject"), panel);
        reject_button_->setObjectName(QStringLiteral("rejectProposalButton"));
        undo_button_ = new QPushButton(QStringLiteral("Undo"), panel);
        first_row->addWidget(accept_button_);
        first_row->addWidget(reject_button_);
        first_row->addWidget(undo_button_);
        layout->addLayout(first_row);

        auto* automatic_row = new QHBoxLayout();
        run_fixed_point_button_ = new QPushButton(
            QStringLiteral("Run certified decomposition"), panel);
        run_fixed_point_button_->setObjectName(
            QStringLiteral("resolveCertifiedShapesButton"));
        run_fixed_point_button_->setToolTip(QStringLiteral(
            "Resolve every decomposition the engine can certify; uncertain "
            "residuals remain unchanged for review"));
        cancel_fixed_point_button_ = new QPushButton(QStringLiteral("Cancel"), panel);
        cancel_fixed_point_button_->setObjectName(
            QStringLiteral("cancelGeometryJobButton"));
        cancel_fixed_point_button_->setEnabled(false);
        automatic_row->addWidget(run_fixed_point_button_, 1);
        automatic_row->addWidget(cancel_fixed_point_button_);
        layout->addLayout(automatic_row);
        fixed_point_progress_ = new QProgressBar(panel);
        fixed_point_progress_->setRange(0, 1);
        fixed_point_progress_->setValue(0);
        fixed_point_progress_->setFormat(QStringLiteral("Ready"));
        layout->addWidget(fixed_point_progress_);
        fixed_point_status_ = new QLabel(
            QStringLiteral("Certified automatic actions do not require authorization."), panel);
        fixed_point_status_->setObjectName(QStringLiteral("geometryJobStatus"));
        fixed_point_status_->setWordWrap(true);
        layout->addWidget(fixed_point_status_);
        review_residual_button_ = new QPushButton(
            QStringLiteral("Review selected residual"), panel);
        review_residual_button_->setObjectName(
            QStringLiteral("generateDecompositionOptionsButton"));
        layout->addWidget(review_residual_button_);

        auto* second_row = new QHBoxLayout();
        auto* fit_button = new QPushButton(QStringLiteral("Fit"), panel);
        auto* isolate_button = new QPushButton(QStringLiteral("Isolate"), panel);
        auto* context_button = new QPushButton(QStringLiteral("Local context"), panel);
        auto* show_button = new QPushButton(QStringLiteral("Show all"), panel);
        auto* grid_button = new QPushButton(QStringLiteral("Grid"), panel);
        auto* axes_button = new QPushButton(QStringLiteral("Axes"), panel);
        grid_button->setObjectName(QStringLiteral("referenceGridButton"));
        axes_button->setObjectName(QStringLiteral("referenceAxesButton"));
        grid_button->setCheckable(true);
        axes_button->setCheckable(true);
        grid_button->setChecked(true);
        axes_button->setChecked(true);
        grid_button->setToolTip(QStringLiteral(
            "Show an adaptive XY reference floor below the model"));
        axes_button->setToolTip(QStringLiteral(
            "Show the color-coded XYZ orientation triad"));
        second_row->addWidget(fit_button);
        second_row->addWidget(isolate_button);
        second_row->addWidget(context_button);
        second_row->addWidget(show_button);
        second_row->addStretch();
        viewport_layout_->insertLayout(1, second_row);
        auto* reference_row = new QHBoxLayout;
        reference_row->addWidget(grid_button);
        reference_row->addWidget(axes_button);
        auto* presentation_legend = new QLabel(
            QStringLiteral("Cyan: selected   Green: exact preview\n"
                           "Orange: residual   Magenta: sliver"),
            panel);
        presentation_legend->setWordWrap(true);
        presentation_legend->setProperty("role", "secondary");
        presentation_legend->setToolTip(QStringLiteral(
            "Cyan: selection; green: exact preview; orange: residual/review; "
            "magenta: sliver; red: removal/blocker; yellow: active job; "
            "gray: context/provisional"));
        reference_row->addWidget(presentation_legend, 1);
        viewport_layout_->addLayout(reference_row);
        auto* navigation_help = new QLabel(
            QStringLiteral("Click: select  ·  Left-drag: rotate  ·  "
                           "Middle-drag: pan  ·  Wheel: zoom  ·  F: fit"),
            panel);
        navigation_help->setWordWrap(true);
        navigation_help->setAccessibleName(QStringLiteral("Viewport controls"));
        navigation_help->setProperty("role", "secondary");
        viewport_layout_->addWidget(navigation_help);

        commit_button_ = new QPushButton(
            commit_path_.empty() ? QStringLiteral("Close")
                                 : QStringLiteral("Emit and close…"), panel);
        commit_button_->setObjectName(QStringLiteral("emitAndCloseButton"));
        layout->addWidget(commit_button_);

        connect(accept_button_, &QPushButton::clicked, this, [this]() { accept(); });
        connect(reject_button_, &QPushButton::clicked, this, [this]() { reject(); });
        connect(undo_button_, &QPushButton::clicked, this, [this]() { undo(); });
        connect(run_fixed_point_button_, &QPushButton::clicked, this,
                [this]() { runFixedPoint(); });
        connect(cancel_fixed_point_button_, &QPushButton::clicked, this, [this]() {
            if (!job_controller_ || !job_controller_->active()) return;
            requestProjectRemovalCancellation();
            job_controller_->cancel();
            cancel_fixed_point_button_->setEnabled(false);
            fixed_point_status_->setText(QStringLiteral(
                "Cancellation requested; the current OCCT operation will finish safely."));
            appendHistory(removal_running_ ? "batch-removal cancellation requested"
                                           : "geometry-job cancellation requested");
        });
        connect(select_removals_button_, &QPushButton::clicked, this,
                [this]() { selectEligibleRemovals(); });
        connect(review_residual_button_, &QPushButton::clicked, this,
                [this]() { prepareResidualReview(); });
        connect(fit_button, &QPushButton::clicked, viewport_, &OcctViewWidget::fitAll);
        connect(isolate_button, &QPushButton::clicked, this, [this]() {
            if (!selected_id_.empty()) viewport_->isolate(selected_id_);
        });
        connect(context_button, &QPushButton::clicked, this, [this]() {
            if (!selected_id_.empty()) viewport_->showLocalContext(selected_id_);
        });
        connect(show_button, &QPushButton::clicked, this, [this]() {
            if (focused_initial_display_) {
                focused_initial_display_ = false;
                refreshDisplayedGeometry();
            }
            viewport_->showAll();
        });
        connect(grid_button, &QPushButton::toggled,
                viewport_, &OcctViewWidget::setGridVisible);
        connect(axes_button, &QPushButton::toggled,
                viewport_, &OcctViewWidget::setTrihedronVisible);
        connect(commit_button_, &QPushButton::clicked, this, &QWidget::close);
        connect(before_button_, &QRadioButton::clicked, this, [this]() {
            setDisplayState(DisplayState::before);
        });
        connect(preview_button_, &QRadioButton::clicked, this, [this]() {
            const DecompositionCandidate* candidate = selectedCandidate();
            if (!candidate) {
                setDisplayState(DisplayState::before);
            } else if (!preview_.ok || preview_.candidate_id != candidate->id) {
                loadPreview(candidate);
            } else {
                setDisplayState(DisplayState::preview);
            }
        });
        connect(applied_button_, &QRadioButton::clicked, this, [this]() {
            setDisplayState(DisplayState::applied);
        });
        return panel;
    }

    void configureDecisionControls() {
        const std::string& kind = controller_.review().kind;
        const bool inspection = kind.empty();
        const bool decomposition = kind == "decomposition";
        const bool removal = removalReviewMode();
        inspector_tabs_->setTabVisible(0, decomposition || removal);
        if (inspection || (!decomposition && !removal))
            inspector_tabs_->setCurrentIndex(1);
        else
            inspector_tabs_->setCurrentIndex(0);

        accept_button_->show();
        reject_button_->show();
        undo_button_->show();
        before_button_->show();
        preview_button_->show();
        applied_button_->show();

        if (inspection) {
            accept_button_->hide();
            reject_button_->hide();
            undo_button_->setVisible(inspection_has_mutation_);
            before_button_->hide();
            preview_button_->hide();
            applied_button_->hide();
        } else if (decomposition) {
            accept_button_->setText(QStringLiteral("Apply decomposition…"));
            reject_button_->setText(QStringLiteral("Reject proposal"));
        } else if (kind == "surface_replacement") {
            accept_button_->setText(QStringLiteral("Apply surface replacement…"));
            reject_button_->setText(QStringLiteral("Reject proposal"));
            before_button_->hide();
            preview_button_->hide();
            applied_button_->hide();
        } else if (kind == "hole_fill") {
            accept_button_->setText(QStringLiteral("Fill hole…"));
            reject_button_->setText(QStringLiteral("Reject proposal"));
            before_button_->hide();
            preview_button_->hide();
            applied_button_->hide();
        } else if (kind == "contained_medium") {
            accept_button_->setText(QStringLiteral("Generate contained medium…"));
            reject_button_->setText(QStringLiteral("Reject proposal"));
            before_button_->hide();
            preview_button_->hide();
            applied_button_->hide();
        }

        if (removal) preview_button_->hide();

        const bool show_fixed_point = (inspection || decomposition)
            && !decision_callback_;
        run_fixed_point_button_->setVisible(show_fixed_point);
        cancel_fixed_point_button_->setVisible(show_fixed_point);
        fixed_point_progress_->setVisible(show_fixed_point);
        fixed_point_status_->setVisible(show_fixed_point);
        review_residual_button_->setVisible(show_fixed_point);
        if (inspection) {
            review_residual_button_->setText(
                QStringLiteral("Generate decomposition options"));
            fixed_point_progress_->setRange(0, 1);
            fixed_point_progress_->setValue(0);
            fixed_point_progress_->setFormat(QStringLiteral("Ready"));
            fixed_point_status_->setText(QStringLiteral(
                "Only engine-certified changes are applied automatically; "
                "uncertain shapes remain unchanged for review."));
        } else if (decomposition) {
            review_residual_button_->setText(
                QStringLiteral("Generate new options for selected residual"));
        }
        if (accept_shortcut_) {
            accept_shortcut_->setEnabled(!inspection && !removal);
        }
        updateDecompositionActionState();
    }

    void populateRemovalPlan() {
        removal_table_updating_ = true;
        candidates_->clearContents();
        candidates_->setRowCount(static_cast<int>(removal_plan_.size()));
        const bool single_explicit = removal_plan_.size() == 1
            && controller_.review().kind == "removal";
        for (int row = 0; row < static_cast<int>(removal_plan_.size()); ++row) {
            const QtRemovalPlanEntry& entry = removal_plan_[row];
            auto* select = new QTableWidgetItem;
            select->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable
                             | Qt::ItemIsUserCheckable);
            if (entry.protected_target)
                select->setFlags(Qt::ItemIsSelectable);
            select->setCheckState((entry.auto_eligible || single_explicit)
                                  && !entry.protected_target
                                  && !entry.previously_rejected
                ? Qt::Checked : Qt::Unchecked);
            select->setData(Qt::UserRole, row);
            const QString component = entry.component_id.empty()
                ? QStringLiteral("Ungrouped solid")
                : QString::fromStdString(entry.component_id);
            const QString label = entry.label.empty()
                ? component : component + QStringLiteral(" — ")
                    + QString::fromStdString(entry.label);
            const QString status = entry.protected_target
                ? QStringLiteral("PROTECTED: %1").arg(joined(entry.blockers))
                : entry.previously_rejected
                    ? QStringLiteral(
                        "Previously rejected — select only to reconsider")
                : entry.blockers.empty()
                    ? joined(entry.reason_codes)
                    : QStringLiteral("Review: %1").arg(joined(entry.blockers));
            candidates_->setItem(row, 0, select);
            candidates_->setItem(row, 1, new QTableWidgetItem(label));
            candidates_->item(row, 1)->setToolTip(QStringLiteral(
                "Occurrence: %1\nDefinition: %2\nTargets: %3")
                .arg(component,
                     entry.definition_id.empty() ? QStringLiteral("unknown")
                        : QString::fromStdString(entry.definition_id),
                     joined(entry.target_ids)));
            candidates_->setItem(row, 2, new QTableWidgetItem(
                QString::number(entry.target_ids.size())));
            candidates_->setItem(row, 3, new QTableWidgetItem(
                QString::fromStdString(entry.confidence)));
            candidates_->setItem(row, 4, new QTableWidgetItem(
                number(100.0 * entry.volume_fraction)));
            candidates_->setItem(row, 5, new QTableWidgetItem(status));
            for (int column = 0; column < 6; ++column) {
                if (entry.protected_target || entry.previously_rejected)
                    candidates_->item(row, column)->setForeground(
                        entry.protected_target ? QColor(210, 80, 70)
                                               : QColor(150, 95, 45));
            }
        }
        removal_table_updating_ = false;
        if (!removal_table_connected_) {
            connect(candidates_, &QTableWidget::itemChanged, this,
                    [this](QTableWidgetItem*) {
                        if (!removal_table_updating_)
                            updateRemovalPlanPresentation();
                    });
            removal_table_connected_ = true;
        }
        candidates_->show();
        preview_button_->setEnabled(false);
        applied_button_->setEnabled(false);
        accept_button_->setText(QStringLiteral("Remove selected…"));
        reject_button_->setText(QStringLiteral("Reject plan"));
        updateRemovalPlanPresentation();
    }

    std::vector<std::string> selectedRemovalTargets() const {
        std::set<std::string> selected;
        for (int row = 0; row < candidates_->rowCount(); ++row) {
            const QTableWidgetItem* item = candidates_->item(row, 0);
            if (!item || item->checkState() != Qt::Checked) continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= static_cast<int>(removal_plan_.size()))
                continue;
            for (const std::string& target : removal_plan_[index].target_ids)
                selected.insert(target);
        }
        return {selected.begin(), selected.end()};
    }

    std::vector<RemovalPlanArtifactEntry> selectedRemovalArtifactEntries() const {
        std::vector<RemovalPlanArtifactEntry> entries;
        for (int row = 0; row < candidates_->rowCount(); ++row) {
            const QTableWidgetItem* item = candidates_->item(row, 0);
            if (!item || item->checkState() != Qt::Checked) continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= static_cast<int>(removal_plan_.size()))
                continue;
            const QtRemovalPlanEntry& selected = removal_plan_[index];
            entries.push_back({selected.component_id,
                selected.definition_id, selected.target_ids});
        }
        return entries;
    }

    void updateRemovalPlanPresentation() {
        if (!removalReviewMode() || removal_running_) return;
        const std::vector<std::string> targets = selectedRemovalTargets();
        double fraction = 0.0;
        std::size_t components = 0;
        for (int row = 0; row < candidates_->rowCount(); ++row) {
            const QTableWidgetItem* item = candidates_->item(row, 0);
            if (!item || item->checkState() != Qt::Checked) continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(removal_plan_.size())) {
                fraction += removal_plan_[index].volume_fraction;
                ++components;
            }
        }
        summary_->setText(QStringLiteral(
            "Removal plan: %1 component(s), %2 exact solid target(s)\n"
            "Combined document volume: %3% · base revision: %4")
            .arg(components).arg(targets.size())
            .arg(number(100.0 * fraction))
            .arg(removal_base_revision_));
        accept_button_->setText(QStringLiteral("Remove %1 %2 (%3 %4)…")
            .arg(components)
            .arg(components == 1 ? QStringLiteral("component")
                                 : QStringLiteral("components"))
            .arg(targets.size())
            .arg(targets.size() == 1 ? QStringLiteral("solid")
                                     : QStringLiteral("solids")));
        accept_button_->setEnabled(!targets.empty());
        job_highlights_.clear();
        for (const std::string& target : targets)
            job_highlights_[target] = JobHighlightRole::removal_candidate;
        viewport_->setJobHighlights(job_highlights_);
    }

    void selectEligibleRemovals() {
        removal_table_updating_ = true;
        for (int row = 0; row < candidates_->rowCount(); ++row) {
            QTableWidgetItem* item = candidates_->item(row, 0);
            if (!item) continue;
            const int index = item->data(Qt::UserRole).toInt();
            const bool eligible = index >= 0
                && index < static_cast<int>(removal_plan_.size())
                && removal_plan_[index].auto_eligible;
            item->setCheckState(eligible ? Qt::Checked : Qt::Unchecked);
        }
        removal_table_updating_ = false;
        updateRemovalPlanPresentation();
    }

    void populateCandidates(const std::string& requested) {
        candidates_->setRowCount(0);
        if (removalReviewMode()) {
            populateRemovalPlan();
            return;
        }
        if (!proposal_) {
            candidates_->hide();
            return;
        }
        candidates_->setRowCount(static_cast<int>(proposal_->candidates.size()));
        int selected_row = -1;
        for (int row = 0; row < static_cast<int>(proposal_->candidates.size()); ++row) {
            const DecompositionCandidate& c = proposal_->candidates[row];
            const QString values[] = {
                QString::fromStdString(c.id), QString::fromStdString(c.confidence),
                QString::number(c.piece_count), QString::number(c.exact_piece_count),
                QString::number(c.pieces_requiring_decomposition), number(c.relative_delta)};
            for (int column = 0; column < 6; ++column) {
                auto* item = new QTableWidgetItem(values[column]);
                item->setData(Qt::UserRole, QString::fromStdString(c.id));
                if (column >= 2)
                    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                if (!c.usable) item->setForeground(QColor(190, 70, 60));
                candidates_->setItem(row, column, item);
            }
            if (c.id == requested
                || (requested.empty() && c.id == proposal_->best_candidate_id)) {
                selected_row = row;
            }
        }
        if (!candidates_connected_) {
            connect(candidates_, &QTableWidget::itemSelectionChanged, this, [this]() {
                const int row = candidates_->currentRow();
                if (row < 0 || !proposal_) return;
                const std::string id = candidates_->item(row, 0)
                                           ->data(Qt::UserRole).toString().toStdString();
                const ViewerAction action = controller_.select_candidate(id);
                appendAction(action);
                const DecompositionCandidate* candidate = find_candidate(proposal_, id);
                viewport_->showCandidate(candidate);
                preview_ = {};
                preview_button_->setEnabled(candidate && candidate->usable);
                setDisplayState(DisplayState::before);
                updateDetails();
            });
            candidates_connected_ = true;
        }
        if (selected_row < 0 && !proposal_->candidates.empty()) selected_row = 0;
        if (selected_row >= 0) candidates_->selectRow(selected_row);
    }

    void refreshGeometry() {
        current_entries_ = collect_view_entries(session_);
        if (before_entries_.empty()) before_entries_ = current_entries_;
        refreshDisplayedGeometry();
    }

    std::vector<SolidViewEntry> displayedEntries() const {
        if (display_state_ == DisplayState::applied) return current_entries_;
        std::vector<SolidViewEntry> entries = before_entries_;
        if (display_state_ != DisplayState::preview || !preview_.ok) {
            return entries;
        }
        // Keep the parent visible beneath the coloured children. This makes
        // outside/coverage failures visually inspectable instead of showing
        // only an apparently plausible replacement in isolation.
        entries.insert(entries.end(), preview_.pieces.begin(), preview_.pieces.end());
        return entries;
    }

    void refreshDisplayedGeometry() {
        std::vector<SolidViewEntry> entries = displayedEntries();
        if (focused_initial_display_ && !selected_id_.empty()) {
            std::vector<SolidViewEntry> focused = entries;
            focused.erase(std::remove_if(focused.begin(), focused.end(),
                [this](const SolidViewEntry& entry) {
                    return !id_matches(entry.shape_id, selected_id_)
                        && !id_matches(entry.source_solid_id, selected_id_);
                }), focused.end());
            if (!focused.empty()) entries = std::move(focused);
            else focused_initial_display_ = false;
        }
        tree_->clear();
        auto* root = new QTreeWidgetItem(tree_,
            {QString::fromStdString(document_id_), QStringLiteral("Document")});
        std::map<std::string, QTreeWidgetItem*> source_nodes;
        for (const SolidViewEntry& entry : entries) {
            QTreeWidgetItem* parent = root;
            if (!entry.source_solid_id.empty()) {
                auto found = source_nodes.find(entry.source_solid_id);
                if (found == source_nodes.end()) {
                    found = source_nodes.emplace(entry.source_solid_id,
                        new QTreeWidgetItem(root,
                            {QString::fromStdString(entry.source_solid_id),
                             QStringLiteral("Source")})).first;
                }
                parent = found->second;
            }
            const QString label = entry.name.empty()
                ? QString::fromStdString(entry.shape_id)
                : QString::fromStdString(entry.shape_id + " — " + entry.name);
            QString entry_status = status_text(entry.status);
            if (entry.overlap_conflict)
                entry_status += QStringLiteral(" · OVERLAP");
            auto* item = new QTreeWidgetItem(parent, {label, entry_status});
            item->setData(0, Qt::UserRole, QString::fromStdString(entry.shape_id));
            item->setToolTip(0, label);
            item->setToolTip(1, entry_status);
        }
        root->setExpanded(true);
        filterModelTree();
        viewport_->setEntries(entries, selected_id_);
        selectTreeId(selected_id_);
    }

    void setDisplayState(DisplayState state) {
        if (state == DisplayState::preview && !preview_.ok) {
            appendHistory("preview unavailable for the selected candidate");
            state = DisplayState::before;
        }
        display_state_ = state;
        before_button_->setChecked(state == DisplayState::before);
        preview_button_->setChecked(state == DisplayState::preview);
        applied_button_->setChecked(state == DisplayState::applied);
        refreshDisplayedGeometry();
    }

    void loadPreview(const DecompositionCandidate* candidate) {
        preview_ = {};
        if (!candidate) {
            preview_button_->setEnabled(false);
            setDisplayState(DisplayState::before);
            return;
        }
        if (job_controller_->active()) {
            appendHistory("preview deferred: another geometry job is active");
            return;
        }
        preview_running_ = true;
        pending_terminal_event_ = {};
        artifact_capture_ = {};
        job_highlights_.clear();
        job_dock_->beginJob();
        job_dock_->show();
        cancel_fixed_point_button_->setEnabled(true);
        run_fixed_point_button_->setEnabled(false);
        accept_button_->setEnabled(false);
        reject_button_->setEnabled(false);
        undo_button_->setEnabled(false);
        candidates_->setEnabled(false);
        before_button_->setEnabled(false);
        preview_button_->setEnabled(false);
        applied_button_->setEnabled(false);
        review_residual_button_->setEnabled(false);
        appendHistory("started non-mutating preview on a worker thread");
        if (!job_controller_->startPreview(session_, document_id_, *candidate)) {
            preview_running_ = false;
            cancel_fixed_point_button_->setEnabled(false);
            run_fixed_point_button_->setEnabled(true);
            accept_button_->setEnabled(true);
            reject_button_->setEnabled(true);
            undo_button_->setEnabled(true);
            candidates_->setEnabled(true);
            before_button_->setEnabled(true);
            preview_button_->setEnabled(true);
            applied_button_->setEnabled(true);
            review_residual_button_->setEnabled(true);
            appendHistory("could not start preview worker");
        }
    }

    void previewFinished(const DecompositionPreview& result) {
        preview_ = result;
        if (preview_.ok) {
            appendHistory("generated non-mutating preview for "
                + preview_.candidate_id + " ("
                + std::to_string(preview_.pieces.size()) + " pieces)");
            setDisplayState(DisplayState::preview);
        } else {
            appendHistory("could not generate preview for "
                + preview_.candidate_id);
            setDisplayState(DisplayState::before);
        }
        if (pending_terminal_event_.phase == JobPhase::finished) {
            artifact_capture_.capture(
                preview_.ok ? QStringLiteral("preview-ready.png")
                            : QStringLiteral("preview-failed.png"),
                [this](const QString& path) { return viewport_->dumpPng(path); });
            artifact_capture_.writeSummary(pending_terminal_event_);
        }
        updateDetails();
        inspector_tabs_->setCurrentIndex(1);
    }

    const DecompositionCandidate* selectedCandidate() const {
        if (!proposal_ || !candidates_ || candidates_->currentRow() < 0)
            return nullptr;
        const QTableWidgetItem* item = candidates_->item(candidates_->currentRow(), 0);
        if (!item) return nullptr;
        return find_candidate(proposal_, item->data(Qt::UserRole)
            .toString().toStdString());
    }

    void selectTreeId(const std::string& id) {
        if (id.empty()) return;
        const auto items = tree_->findItems(QStringLiteral("*"),
            Qt::MatchWildcard | Qt::MatchRecursive, 0);
        for (QTreeWidgetItem* item : items) {
            const std::string item_id = item->data(0, Qt::UserRole)
                                            .toString().toStdString();
            if (id_matches(item_id, id)) {
                tree_->setCurrentItem(item);
                return;
            }
        }
    }

    static bool needsDecompositionAction(SolidViewStatus status) {
        return status == SolidViewStatus::needs_decomposition
            || status == SolidViewStatus::no_interior_point
            || status == SolidViewStatus::preview_residual;
    }

    const SolidViewEntry* selectedEntry() const {
        for (const SolidViewEntry& entry : current_entries_) {
            if (id_matches(entry.shape_id, selected_id_)) return &entry;
        }
        return nullptr;
    }

    std::size_t unresolvedShapeCount() const {
        return static_cast<std::size_t>(std::count_if(
            current_entries_.begin(), current_entries_.end(),
            [](const SolidViewEntry& entry) {
                return needsDecompositionAction(entry.status);
            }));
    }

    void updateDecompositionActionState() {
        if (!run_fixed_point_button_ || !review_residual_button_) return;
        const std::string& kind = controller_.review().kind;
        if (kind != "decomposition" && !kind.empty()) return;

        const std::size_t unresolved = unresolvedShapeCount();
        const SolidViewEntry* entry = selectedEntry();
        const bool selected_unresolved = entry
            && needsDecompositionAction(entry->status);
        const bool busy = fixed_point_running_ || residual_review_running_
            || (job_controller_ && job_controller_->active());

        run_fixed_point_button_->setText(
            QStringLiteral("Resolve all certifiable shapes (%1 unresolved)")
                .arg(unresolved));
        run_fixed_point_button_->setEnabled(!busy && unresolved > 0);
        review_residual_button_->setEnabled(!busy && selected_unresolved);
        if (selected_unresolved) {
            review_residual_button_->setToolTip(QStringLiteral(
                "Generate bounded decomposition candidates for %1")
                .arg(QString::fromStdString(entry->shape_id)));
        } else {
            review_residual_button_->setToolTip(QStringLiteral(
                "Select a shape marked Needs decomposition first"));
        }
    }

    void selectionChanged() {
        updateDecompositionActionState();
        updateDetails();
    }

    void filterModelTree() {
        const QString query = model_search_->text().trimmed();
        std::size_t matches = 0;
        const auto visit = [&](const auto& self, QTreeWidgetItem* item,
                               bool ancestor_matches) -> bool {
            const bool own_match = query.isEmpty()
                || item->text(0).contains(query, Qt::CaseInsensitive)
                || item->text(1).contains(query, Qt::CaseInsensitive);
            const bool include_children = ancestor_matches || own_match;
            bool visible_child = false;
            for (int i = 0; i < item->childCount(); ++i)
                visible_child = self(self, item->child(i), include_children)
                    || visible_child;
            const bool visible = include_children || visible_child;
            item->setHidden(!visible);
            if (!query.isEmpty() && visible_child) item->setExpanded(true);
            if (visible && item->childCount() == 0) ++matches;
            return visible;
        };
        for (int i = 0; i < tree_->topLevelItemCount(); ++i)
            visit(visit, tree_->topLevelItem(i), false);
        search_status_->setText(query.isEmpty()
            ? QStringLiteral("%1 shapes · select to inspect").arg(matches)
            : QStringLiteral("%1 matching shapes").arg(matches));
    }

    void updateDetails() {
        const ViewerReviewContext& review = controller_.review();
        const QString review_title = review.kind.empty()
            ? QStringLiteral("Inspect geometry")
            : QString::fromStdString(review.kind).replace('_', ' ');
        title_->setText(review.kind.empty() ? review_title
            : QStringLiteral("Review %1").arg(review_title));
        workspace_status_->setText(QStringLiteral(
            "Revision %1   ·   %2 %3   ·   %4 need decomposition")
            .arg(document_revision(session_, document_id_))
            .arg(current_entries_.size())
            .arg(current_entries_.size() == 1 ? QStringLiteral("shape")
                                              : QStringLiteral("shapes"))
            .arg(unresolvedShapeCount()));
        if (removalReviewMode()) {
            updateRemovalPlanPresentation();
        } else if (review.kind.empty()) {
            const SolidViewEntry* entry = selectedEntry();
            if (entry) {
                const QString label = !entry->name.empty()
                    ? QString::fromStdString(entry->name)
                    : (!entry->label.empty() ? QString::fromStdString(entry->label)
                                             : QString::fromStdString(entry->shape_id));
                const QString next = needsDecompositionAction(entry->status)
                    ? QStringLiteral("Generate decomposition options or resolve all certifiable shapes")
                    : QStringLiteral("No corrective action required for this shape");
                summary_->setText(QStringLiteral(
                    "Selection: %1\nStatus: %2\nNext: %3")
                    .arg(label, status_text(entry->status), next));
            } else {
                summary_->setText(QStringLiteral(
                    "Select an unresolved shape to inspect it and generate a safe proposal."));
            }
        } else {
            summary_->setText(QStringLiteral("Target: %1\nProposed change: %2\nCandidate: %3")
                .arg(QString::fromStdString(review.target_id),
                     review_title,
                     QString::fromStdString(review.candidate_id)));
        }

        QString html;
        if (review.kind.empty()) {
            const std::size_t unresolved = unresolvedShapeCount();
            const SolidViewEntry* entry = selectedEntry();
            html += QStringLiteral(
                "<div style='border:1px solid #d7a900;padding:8px'>"
                "<b>%1 unresolved shape(s)</b><br>"
                "Resolve all changes that pass exact certification, or select "
                "one unresolved shape and generate alternatives for review. "
                "Uncertain geometry is never changed automatically.</div>")
                .arg(unresolved);
            if (entry) {
                const QString label = (!entry->name.empty()
                    ? QString::fromStdString(entry->name)
                    : (!entry->label.empty() ? QString::fromStdString(entry->label)
                                             : QString::fromStdString(entry->shape_id)))
                    .toHtmlEscaped();
                html += QStringLiteral("<h3>Selected shape</h3>%1<br>ID: %2<br>Status: %3")
                    .arg(label,
                         QString::fromStdString(entry->shape_id).toHtmlEscaped(),
                         status_text(entry->status));
                if (needsDecompositionAction(entry->status)) {
                    html += QStringLiteral(
                        "<br><b>Recommended:</b> Generate decomposition options "
                        "to compare a certified preview before applying a manual choice.");
                }
            } else {
                html += QStringLiteral(
                    "<h3>Next action</h3>Select a row marked "
                    "<b>Needs decomposition</b> in the model tree.");
            }
        }
        if (!review.audit.proposal_id.empty()) {
            html += QStringLiteral(
                "<div style='border:1px solid #d09030;padding:6px'>"
                "<b>Escalated action %1</b><br>"
                "Proposed by: %2<br>Policy gate: %3<br>Rationale: %4<br>"
                "Approval: explicit human decision required</div>")
                .arg(QString::fromStdString(review.audit.proposal_id),
                     QString::fromStdString(review.audit.proposal_actor.empty()
                         ? review.audit.actor : review.audit.proposal_actor),
                     QString::fromStdString(review.audit.policy_rule),
                     QString::fromStdString(review.audit.rationale));
        }
        if (removalReviewMode()) {
            html += QStringLiteral(
                "<h3>Component-removal plan</h3>"
                "Checked rows are resolved to immutable solid IDs at revision %1. "
                "Apply runs the complete selection as one certified transaction "
                "and one undo step.<br>")
                .arg(removal_base_revision_);
            for (const QtRemovalPlanEntry& entry : removal_plan_) {
                const QString component = entry.component_id.empty()
                    ? QStringLiteral("Ungrouped solid")
                    : QString::fromStdString(entry.component_id);
                html += QStringLiteral(
                    "<p><b>%1</b>%2<br>Definition: %3<br>Targets: %4<br>"
                    "Confidence: %5; volume: %6%<br>Reasons: %7<br>"
                    "Blockers: %8</p>")
                    .arg(component,
                         entry.label.empty() ? QString()
                            : QStringLiteral(" — ")
                                + QString::fromStdString(entry.label),
                         entry.definition_id.empty() ? QStringLiteral("unknown")
                            : QString::fromStdString(entry.definition_id),
                         joined(entry.target_ids),
                         QString::fromStdString(entry.confidence),
                         number(100.0 * entry.volume_fraction),
                         entry.reason_codes.empty() ? QStringLiteral("none")
                                                    : joined(entry.reason_codes),
                         entry.blockers.empty() ? QStringLiteral("none")
                                                : joined(entry.blockers));
            }
        } else if (const DecompositionCandidate* c =
                find_candidate(proposal_, review.candidate_id)) {
            html += QStringLiteral("<h3>%1</h3>").arg(QString::fromStdString(c->id));
            html += QStringLiteral("<b>%1 confidence</b> — %2<br>")
                .arg(QString::fromStdString(c->confidence),
                     c->usable ? QStringLiteral("applicable")
                               : QStringLiteral("requires another choice"));
            html += QStringLiteral("Tool: %1<br>Reason: %2<br>")
                .arg(QString::fromStdString(c->tool_kind),
                     QString::fromStdString(c->reason));
            html += QStringLiteral("Pieces: %1; exact: %2; residual: %3; slivers: %4<br>")
                .arg(c->piece_count).arg(c->exact_piece_count)
                .arg(c->pieces_requiring_decomposition).arg(c->sliver_piece_count);
            html += QStringLiteral("Volume: %1 → %2; relative Δ %3<br>")
                .arg(number(c->original_volume), number(c->pieces_volume),
                     number(c->relative_delta));
            html += QStringLiteral("Progress: %1%; balance: %2")
                .arg(number(100.0 * c->progress_ratio), number(c->volume_balance));
            if (preview_.candidate_id == c->id) {
                if (preview_.ok) {
                    html += QStringLiteral(
                        "<hr><b>Regenerated preview</b><br>"
                        "Session revision: %1 (unchanged)<br>"
                        "Shapes held: %2; exact: %3; residual: %4; slivers: %5<br>"
                        "Preview volume: %6<br>"
                        "Summary certificate: %7")
                        .arg(preview_.document_revision)
                        .arg(preview_.pieces.size())
                        .arg(preview_.exact_count)
                        .arg(preview_.residual_count)
                        .arg(preview_.sliver_count)
                        .arg(number(preview_.total_volume))
                        .arg(preview_.summary_matches
                            ? QStringLiteral("passed — count and volume reproduced")
                            : QStringLiteral("failed — do not apply without investigation"));
                    html += QStringLiteral(
                        "<hr><b>Exact local certificate evidence</b><br>"
                        "Partition: %1; valid children: %2; contained: %3; coverage: %4; siblings disjoint: %5<br>"
                        "Relative volume Δ: %6; outside parent: %7; uncovered parent: %8; sibling overlap bound: %9<br>"
                        "OCCT proof calls: %10; failure: %11<br>"
                        "Neighbour safety: %12; unknown: %13; spatial candidates: %14; exact tests: %15; inherited overlaps: %16<br>"
                        "Conflicting body: %17; intersection volume: %18<br>"
                        "<b>Recommended next action:</b> %19")
                        .arg(preview_.partition_exact ? "passed" : "failed")
                        .arg(preview_.valid_children ? "yes" : "no")
                        .arg(preview_.contained_in_parent ? "yes" : "no")
                        .arg(preview_.covers_parent ? "yes" : "no")
                        .arg(preview_.siblings_disjoint ? "yes" : "no")
                        .arg(number(preview_.relative_volume_delta))
                        .arg(number(preview_.outside_parent_volume))
                        .arg(number(preview_.uncovered_parent_volume))
                        .arg(number(preview_.sibling_overlap_volume))
                        .arg(preview_.certificate_boolean_calls)
                        .arg(QString::fromStdString(
                            preview_.certificate_failure_code.empty()
                                ? std::string("none")
                                : preview_.certificate_failure_code))
                        .arg(preview_.overlap_safe ? "passed" : "failed")
                        .arg(preview_.overlap_unknown ? "yes" : "no")
                        .arg(preview_.overlap_spatial_candidates)
                        .arg(preview_.overlap_exact_tests)
                        .arg(preview_.overlap_inherited_hits)
                        .arg(QString::fromStdString(
                            preview_.overlap_conflicting_body_id.empty()
                                ? std::string("none")
                                : preview_.overlap_conflicting_body_id))
                        .arg(number(preview_.overlap_intersection_volume))
                        .arg(QString::fromStdString(
                            preview_.recommended_next_action));
                } else {
                    html += QStringLiteral(
                        "<hr><b style='color:#d06050'>Preview regeneration failed</b>");
                }
            }
            if (!c->rejection_reason.empty()) {
                html += QStringLiteral("<p style='color:#d06050'><b>Warning:</b> %1</p>")
                    .arg(QString::fromStdString(c->rejection_reason));
            }
        } else if (review.has_hole_fill) {
            const HoleFillCandidate& h = review.hole_fill;
            html += QStringLiteral("<b>Hole fill %1</b><br>Confidence: %2<br>"
                                  "Radius: %3<br>Length: %4<br>Plug volume: %5<br>"
                                  "Host volume fraction: %6")
                .arg(QString::fromStdString(h.id), QString::fromStdString(h.confidence),
                     number(h.radius), number(h.length), number(h.plug_volume),
                     number(h.plug_volume_fraction_of_host));
        } else if (review.has_surface_replacement) {
            const SurfaceReplacementCandidate& s = review.surface_replacement;
            html += QStringLiteral("<b>Surface replacement %1</b><br>Kind: %2<br>"
                                  "Confidence: %3<br>Exact: %4<br>Max deviation: %5<br>"
                                  "RMS deviation: %6<br>Volume Δ: %7")
                .arg(QString::fromStdString(s.id), QString::fromStdString(s.kind),
                     QString::fromStdString(s.confidence), s.exact ? "yes" : "no",
                     number(s.max_deviation), number(s.rms_deviation),
                     number(s.owner_relative_volume_delta));
        } else if (review.has_medium) {
            const MediumGenerationProposal& medium = review.medium_proposal;
            html += QStringLiteral(
                "<b>Contained medium %1</b><br>Host: %2<br>Primitive/network: %3<br>"
                "Base radius: %4; top radius: %5<br>Generated volume: %6<br>"
                "Solid valid: %7; closed: %8; leak test: %9; overlap test: %10<br>"
                "Unresolved ports: %11<br>Segments: %12; connections: %13; circuits: %14<br>"
                "Obstacle subtraction: %15; phase coverage: %16; protected interfaces: %17<br>"
                "Liquid: %18; gas: %19; liquid pockets: %20; gas pockets: %21")
                .arg(QString::fromStdString(review.medium_request.id))
                .arg(QString::fromStdString(medium.host_component_id))
                .arg(medium.conical ? QStringLiteral("truncated cone")
                                    : QStringLiteral("cylinder"))
                .arg(number(medium.radius))
                .arg(number(medium.top_radius))
                .arg(number(medium.certificate.generated_volume))
                .arg(medium.certificate.solid_valid ? "yes" : "no")
                .arg(medium.certificate.closed ? "yes" : "no")
                .arg(medium.certificate.leak_test_passed ? "passed" : "failed")
                .arg(medium.certificate.overlap_test_passed ? "passed" : "failed")
                .arg(medium.certificate.unresolved_port_count)
                .arg(medium.certificate.network_segment_count)
                .arg(medium.certificate.network_connection_count)
                .arg(medium.certificate.network_connected_component_count)
                .arg(medium.certificate.obstacle_subtraction_passed ? "passed" : "failed")
                .arg(medium.certificate.phase_coverage_passed ? "passed" : "failed")
                .arg(medium.certificate.protected_interface_passed ? "passed" : "failed")
                .arg(number(medium.certificate.liquid_volume))
                .arg(number(medium.certificate.gas_volume))
                .arg(medium.certificate.liquid_pocket_count)
                .arg(medium.certificate.gas_pocket_count);
        } else if (html.isEmpty()) {
            html = QStringLiteral("Select a solid or review candidate. All accepted "
                                  "mutations use the session API and decision log.");
        }
        const ConversionReport& report = last_report(session_);
        const ApproximationPolicy& policy = report.approximation_policy;
        const ApproximationLedger& ledger = report.approximation_ledger;
        html += QStringLiteral("<hr><h3>Geometry safety</h3>");
        if (policy.enabled) {
            const double baseline = ledger.document_baseline_volume;
            const double used = baseline > 0.0
                ? ledger.cumulative_changed_volume / baseline : 0.0;
            html += QStringLiteral(
                "Approximation budget: %1% global; used: %2%<br>"
                "Added volume: %3; removed volume: %4<br>")
                .arg(number(100.0 * policy.max_global_changed_volume_fraction),
                     number(100.0 * used),
                     number(ledger.cumulative_added_volume),
                     number(ledger.cumulative_removed_volume));
        } else {
            html += QStringLiteral("Approximation: disabled (exact-only)<br>");
        }
        const McnpGeometrySafetyReport& safety = report.mcnp_geometry_safety;
        html += QStringLiteral(
            "MCNP overlap gate: %1 overlap(s), %2 unknown<br>"
            "Modeled-domain coverage: %3")
            .arg(safety.overlap_count).arg(safety.overlap_unknown_count)
            .arg(coverage_text(safety.coverage_status));
        if (!safety.failure_code.empty()) {
            html += QStringLiteral("<br><b style='color:#d06050'>Blocker: %1</b>")
                .arg(QString::fromStdString(safety.failure_code));
        }
        details_->setHtml(html);
    }

    void appendHistory(const std::string& text) {
        history_->appendPlainText(QString::fromStdString(text));
    }

    void appendAction(const ViewerAction& action) {
        if (!action.message.empty()) appendHistory(action.message);
        if (!action.stdout_line.empty()) {
            std::cout << action.stdout_line << '\n';
            std::cout.flush();
        }
    }

    void updateJobProgress(const JobProgressEvent& event) {
        job_dock_->appendEvent(event);
        if (artifact_capture_.directory().isEmpty()) {
            if (artifact_capture_.begin(QString::fromStdString(event.job_id))) {
                job_dock_->setArtifactDirectory(artifact_capture_.directory());
                artifact_capture_.capture(QStringLiteral("initial-isometric.png"),
                    [this](const QString& path) { return viewport_->dumpPng(path); });
            }
        }
        if (removal_running_ && !removal_plan_artifact_written_
            && !artifact_capture_.directory().isEmpty()) {
            removal_plan_artifact_written_ = artifact_capture_.writeRemovalPlan(
                removal_base_revision_, selectedRemovalArtifactEntries(), true);
        }
        artifact_capture_.appendEvent(event);

        if (project_removal_progress_) {
            if (event.phase == JobPhase::finished) {
                project_terminal_event_ = event;
            } else if (!project_removal_progress_->consume(event,
                    project_plan_event_link_pending_
                        ? project_removal_plan_artifact_.digest
                        : std::string{})) {
                appendHistory("project progress requested safe cancellation: "
                    + project_removal_progress_->last_error());
                job_controller_->cancel();
            }
            project_plan_event_link_pending_ = false;
        }

        if (event.message_code == "remove_batch_target_resolved"
            && !event.target_id.empty()) {
            job_highlights_[event.target_id] = JobHighlightRole::removal_candidate;
            if (event.total_known && event.completed == event.total) {
                artifact_capture_.capture(QStringLiteral("removal-plan.png"),
                    [this](const QString& path) { return viewport_->dumpPng(path); });
            }
        } else if (event.message_code == "fixed_point_target_started"
                   || (event.phase == JobPhase::certifying_solids
                       && !event.target_id.empty())) {
            active_job_target_ = event.target_id;
            job_highlights_[event.target_id] = JobHighlightRole::active_target;
        } else if (event.message_code == "fixed_point_action_accepted"
                   && !event.target_id.empty()) {
            job_highlights_[event.target_id] = JobHighlightRole::provisional_processed;
            active_job_target_.clear();
        }
        if (event.message_code == "fixed_point_action_accepted") {
            appendHistory("automatic certified action "
                + std::to_string(event.actions_applied) + ": "
                + event.strategy + " " + event.target_id
                + (event.candidate_id.empty() ? std::string()
                                               : " " + event.candidate_id));
        }
        if (event.phase == JobPhase::finished) {
            pending_terminal_event_ = event;
        }
        pending_visual_progress_ = event;
        if (event.phase == JobPhase::finished) {
            progress_visual_timer_->start(0);
        } else if (!progress_visual_timer_->isActive()) {
            progress_visual_timer_->start();
        }
    }

    void flushProgressVisual() {
        if (!pending_visual_progress_) return;
        const JobProgressEvent event = *pending_visual_progress_;
        pending_visual_progress_.reset();
        viewport_->setJobHighlights(job_highlights_);
        const QString target = event.target_id.empty()
            ? QStringLiteral("—") : QString::fromStdString(event.target_id);
        fixed_point_status_->setText(QStringLiteral(
            "Actions: %1 · residuals: %2 · residual volume: %3 · pieces: %4\n"
            "Target: %5 · elapsed: %6 ms")
            .arg(event.actions_applied).arg(event.residual_count)
            .arg(number(event.residual_volume)).arg(event.active_piece_count)
            .arg(target).arg(number(event.elapsed_ms)));
    }

    void runFixedPoint() {
        if (fixed_point_running_) return;
        fixed_point_running_ = true;
        job_highlights_.clear();
        active_job_target_.clear();
        pending_terminal_event_ = {};
        artifact_capture_ = {};
        removal_plan_artifact_written_ = false;
        job_dock_->beginJob();
        run_fixed_point_button_->setEnabled(false);
        cancel_fixed_point_button_->setEnabled(true);
        accept_button_->setEnabled(false);
        reject_button_->setEnabled(false);
        undo_button_->setEnabled(false);
        candidates_->setEnabled(false);
        before_button_->setEnabled(false);
        preview_button_->setEnabled(false);
        applied_button_->setEnabled(false);
        review_residual_button_->setEnabled(false);
        fixed_point_progress_->setRange(0, 0);
        fixed_point_progress_->setFormat(QStringLiteral("Running certified search…"));
        fixed_point_status_->setText(QStringLiteral("Preparing current document state…"));
        appendHistory("started fixed-point decomposition on a worker thread");

        if (!job_controller_->startFixedPoint(session_, document_id_)) {
            fixed_point_running_ = false;
            run_fixed_point_button_->setEnabled(true);
            cancel_fixed_point_button_->setEnabled(false);
            undo_button_->setEnabled(true);
            before_button_->setEnabled(true);
            applied_button_->setEnabled(true);
            review_residual_button_->setEnabled(true);
            appendHistory("could not start fixed-point job: another writer is active");
        }
    }

    void fixedPointFinished(const FixedPointDecompositionResult& result) {
        fixed_point_running_ = false;
        if (controller_.review().kind.empty()
            && result.summary.actions_applied > 0) {
            inspection_has_mutation_ = true;
            undo_button_->show();
        }
        cancel_fixed_point_button_->setEnabled(false);
        run_fixed_point_button_->setEnabled(true);
        undo_button_->setEnabled(true);
        before_button_->setEnabled(true);
        applied_button_->setEnabled(true);
        review_residual_button_->setEnabled(true);
        fixed_point_progress_->setRange(0, 1);
        fixed_point_progress_->setValue(result.summary.exact ? 1 : 0);
        fixed_point_progress_->setFormat(result.summary.exact
            ? QStringLiteral("Exact") : QStringLiteral("Stopped safely"));
        const QString reason = QString::fromLatin1(
            fixed_point_stop_reason_name(result.summary.stop_reason));
        fixed_point_status_->setText(QStringLiteral(
            "Stopped: %1 · actions: %2 · residuals: %3 · pieces: %4 · elapsed: %5 ms")
            .arg(reason)
            .arg(result.summary.actions_applied)
            .arg(result.summary.final_state.residual_count)
            .arg(result.summary.final_state.active_piece_count)
            .arg(number(result.summary.elapsed_ms)));
        appendHistory("fixed-point decomposition stopped: "
            + std::string(fixed_point_stop_reason_name(result.summary.stop_reason))
            + "; certified actions="
            + std::to_string(result.summary.actions_applied));

        // Any proposal passed into this window predates the worker mutation.
        // Never allow it to be applied to a newer document revision.
        proposal_ = nullptr;
        preview_ = {};
        candidates_->clearContents();
        candidates_->setRowCount(0);
        candidates_->hide();
        accept_button_->setEnabled(false);
        reject_button_->setEnabled(false);
        current_entries_ = collect_view_entries(session_);
        display_state_ = DisplayState::applied;
        before_button_->setChecked(false);
        preview_button_->setChecked(false);
        applied_button_->setChecked(true);
        refreshDisplayedGeometry();
        viewport_->clearJobHighlights();
        job_highlights_.clear();
        if (pending_terminal_event_.phase == JobPhase::finished) {
            const QString image = pending_terminal_event_.outcome == JobOutcome::rolled_back
                || pending_terminal_event_.outcome == JobOutcome::failed
                ? QStringLiteral("rolled-back.png")
                : QStringLiteral("final-certified.png");
            artifact_capture_.capture(image,
                [this](const QString& path) { return viewport_->dumpPng(path); });
            artifact_capture_.writeSummary(pending_terminal_event_);
        }
        updateDetails();
        updateDecompositionActionState();
        if (!result.summary.exact) {
            for (const SolidViewEntry& entry : current_entries_) {
                if (entry.status == SolidViewStatus::needs_decomposition
                    || entry.status == SolidViewStatus::no_interior_point
                    || entry.status == SolidViewStatus::preview_residual) {
                    selected_id_ = entry.shape_id;
                    selectTreeId(selected_id_);
                    viewport_->selectId(selected_id_);
                    selectionChanged();
                    break;
                }
            }
        }
    }

    void prepareResidualReview() {
        if (fixed_point_running_ || residual_review_running_) return;
        if (selected_id_.empty()) {
            QMessageBox::information(this, QStringLiteral("Select a residual"),
                QStringLiteral("Select an unresolved orange solid or piece first."));
            return;
        }
        residual_review_running_ = true;
        review_residual_button_->setEnabled(false);
        run_fixed_point_button_->setEnabled(false);
        fixed_point_progress_->setRange(0, 0);
        fixed_point_progress_->setFormat(QStringLiteral("Preparing residual review…"));
        fixed_point_status_->setText(QStringLiteral(
            "Generating fresh candidates for %1").arg(
                QString::fromStdString(selected_id_)));
        appendHistory("preparing residual review for " + selected_id_);

        const std::string target = selected_id_;
        QPointer<DecisionWindow> guard(this);
        QThread* thread = QThread::create([this, guard, target]() {
            auto proposal = std::make_shared<DecompositionProposal>();
            std::string error;
            try {
                *proposal = propose_decomposition(session_, target);
            } catch (const std::exception& caught) {
                error = caught.what();
            } catch (...) {
                error = "OCCT rejected residual proposal generation";
            }
            if (guard) {
                QMetaObject::invokeMethod(guard,
                    [guard, target, proposal, error]() {
                        if (guard) guard->residualReviewFinished(
                            target, *proposal, error);
                    }, Qt::QueuedConnection);
            }
        });
        residual_review_thread_ = thread;
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    void residualReviewFinished(const std::string& target,
                                const DecompositionProposal& proposal,
                                const std::string& error) {
        residual_review_running_ = false;
        residual_review_thread_.clear();
        review_residual_button_->setEnabled(true);
        run_fixed_point_button_->setEnabled(true);
        fixed_point_progress_->setRange(0, 1);
        fixed_point_progress_->setValue(0);
        fixed_point_progress_->setFormat(QStringLiteral("Human review"));
        if (!error.empty() || !proposal.ok || proposal.candidates.empty()) {
            const QString explanation = !error.empty()
                ? QString::fromStdString(error)
                : QStringLiteral("No decomposition candidates were produced for this residual.");
            fixed_point_status_->setText(explanation);
            appendHistory("residual review unavailable for " + target);
            return;
        }

        generated_proposal_ = proposal;
        proposal_ = &generated_proposal_;
        std::string candidate = generated_proposal_.best_candidate_id;
        if (candidate.empty()) candidate = generated_proposal_.candidates.front().id;
        DecisionAuditContext audit;
        audit.actor = "human";
        audit.proposal_actor = "fixed_point_coordinator";
        audit.proposal_id = candidate;
        audit.rationale = "automatic fixed-point search stopped; residual requires review";
        audit.policy_rule = "decomposition.human_override";
        audit.approval_mode = "manual";
        controller_.configure_decomposition_review(target, candidate, audit);
        populateCandidates(candidate);
        candidates_->show();
        candidates_->setEnabled(true);
        accept_button_->setEnabled(true);
        reject_button_->setEnabled(true);
        configureDecisionControls();
        viewport_->showReviewOverlays(controller_.review());
        fixed_point_status_->setText(QStringLiteral(
            "%1 fresh candidates. Preview the selected trial before approval.")
            .arg(generated_proposal_.candidates.size()));
        appendHistory("prepared fresh residual review for " + target);
        updateDetails();
    }

    void applyAction(const ViewerAction& action) {
        appendAction(action);
        if (action.ok && action.mutated) {
            refreshGeometry();
            setDisplayState(DisplayState::applied);
        }
        updateDetails();
    }

    static std::int64_t epochSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool prepareProjectRemovalJob() {
        if (!project_) return true;
        std::string error;
        std::string source_digest;
        const auto sources = project_->list_sources(10000, 0, &error);
        for (const ProjectSourceSummary& source : sources) {
            if (source.source_path == step_path_) {
                source_digest = source.source_digest;
                break;
            }
        }
        if (source_digest.empty() && sources.size() == 1)
            source_digest = sources.front().source_digest;
        if (!error.empty() || source_digest.empty()) {
            QMessageBox::critical(this, QStringLiteral("Project job unavailable"),
                QStringLiteral("The reviewed source is not indexed in this project: %1")
                    .arg(QString::fromStdString(error.empty()
                        ? step_path_ : error)));
            return false;
        }

        const std::vector<RemovalPlanArtifactEntry> entries =
            selectedRemovalArtifactEntries();
        const std::string plan_json = ViewArtifactCapture::removalPlanJson(
            removal_base_revision_, entries, true);
        ArtifactRecord plan_artifact;
        if (!project_->artifacts().publish_bytes("qt-removal-plan-v1", "json",
                plan_json, &plan_artifact, &error)) {
            QMessageBox::critical(this, QStringLiteral("Project artifact failed"),
                QString::fromStdString(error));
            return false;
        }

        static std::atomic<std::uint64_t> next_project_removal{0};
        const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ProjectJobSpec spec;
        spec.job_id = "qt-remove:" + std::to_string(tick) + ":"
            + std::to_string(++next_project_removal);
        spec.target_id = document_id_;
        spec.operation = "qt_batch_remove_v1";
        spec.input_digest = source_digest;
        spec.policy_digest = plan_artifact.digest;
        spec.stable_order = tick;
        spec.max_attempts = 1;
        if (!project_->enqueue_job(spec, {}, &error)) {
            QMessageBox::critical(this, QStringLiteral("Project job failed"),
                QString::fromStdString(error));
            return false;
        }
        const std::string worker_id = "qt-review:" + spec.job_id;
        auto claimed = project_->claim_job(
            spec.job_id, worker_id, epochSeconds(), &error);
        if (!claimed) {
            QMessageBox::critical(this, QStringLiteral("Project job failed"),
                QString::fromStdString(error));
            return false;
        }
        project_removal_job_ = *claimed;
        if (!project_->record_artifact(plan_artifact, &error)) {
            project_->fail_job(spec.job_id, worker_id,
                "removal_plan_artifact_failed", error, false,
                epochSeconds(), nullptr);
            project_removal_job_.reset();
            QMessageBox::critical(this, QStringLiteral("Project artifact failed"),
                QString::fromStdString(error));
            return false;
        }
        project_removal_plan_artifact_ = plan_artifact;
        project_removal_progress_ = std::make_unique<ProjectJobProgressSink>(
            *project_, *project_removal_job_);
        if (!project_removal_progress_->ready()) {
            const std::string sink_error = project_removal_progress_->last_error();
            project_->fail_job(spec.job_id, worker_id,
                "project_progress_sink_failed", sink_error, false,
                epochSeconds(), nullptr);
            project_removal_progress_.reset();
            project_removal_job_.reset();
            QMessageBox::critical(this, QStringLiteral("Project progress failed"),
                QString::fromStdString(sink_error));
            return false;
        }
        project_terminal_event_ = {};
        project_removal_finalized_ = false;
        project_plan_event_link_pending_ = true;
        appendHistory("created project job " + spec.job_id);
        return true;
    }

    void requestProjectRemovalCancellation() {
        if (!project_ || !project_removal_job_ || project_removal_finalized_)
            return;
        std::string error;
        if (!project_->request_job_cancellation(
                project_removal_job_->spec.job_id, epochSeconds(), &error)
            && !error.empty()) {
            appendHistory("project cancellation request failed: " + error);
        }
    }

    void failProjectRemoval(const std::string& code,
                            const std::string& message) {
        if (!project_ || !project_removal_job_ || project_removal_finalized_)
            return;
        if (project_removal_progress_
            && project_terminal_event_.phase == JobPhase::finished
            && !project_removal_progress_->terminal_seen()) {
            JobProgressEvent failed_event = project_terminal_event_;
            if (failed_event.outcome == JobOutcome::committed) {
                failed_event.outcome = JobOutcome::failed;
                failed_event.message_code = code;
            }
            project_removal_progress_->consume(failed_event);
        }
        std::string error;
        project_->fail_job(project_removal_job_->spec.job_id,
            project_removal_job_->worker_id, code, message, false,
            epochSeconds(), &error);
        if (!error.empty()) appendHistory("project job finalization failed: " + error);
        project_removal_finalized_ = true;
    }

    void lockForPublicationRecovery(const std::string& message) {
        publication_recovery_required_ = true;
        removal_completed_success_ = false;
        accept_button_->setEnabled(false);
        reject_button_->setEnabled(false);
        candidates_->setEnabled(false);
        select_removals_button_->setEnabled(false);
        run_fixed_point_button_->setEnabled(false);
        review_residual_button_->setEnabled(false);
        if (accept_shortcut_) accept_shortcut_->setEnabled(false);
        undo_button_->setEnabled(true);
        undo_button_->setText(QStringLiteral("Retry recovery"));
        workspace_status_->setText(QStringLiteral(
            "RECOVERY REQUIRED — the project could not publish this change. "
            "Further edits are locked; retry recovery before closing."));
        fixed_point_status_->setText(QString::fromStdString(message));
        appendHistory("publication recovery required: " + message);
    }

    void restoreRemovalReviewsToPending() {
        DecisionAuditContext audit = controller_.review().audit;
        if (audit.actor.empty()) audit.actor = "human";
        audit.approval_mode = "manual";
        audit.rationale = "certified change was rolled back because project publication failed";
        for (const QtRemovalPlanEntry& entry : removal_plan_) {
            for (std::size_t index = 0; index < entry.target_ids.size(); ++index) {
                if (std::find(removal_targets_.begin(), removal_targets_.end(),
                        entry.target_ids[index]) == removal_targets_.end()) continue;
                const std::string proposal_id = index < entry.review_ids.size()
                    && !entry.review_ids[index].empty()
                    ? entry.review_ids[index] : entry.target_ids[index];
                audit.proposal_id = proposal_id;
                record_decision_review(session_, entry.target_ids[index],
                    proposal_id, "pending", audit, controller_.review().kind,
                    "remove_solid", {}, document_revision(session_, document_id_));
            }
        }
    }

    bool recoverFailedProjectRemoval(const std::string& code,
                                     const std::string& message,
                                     bool show_message = true) {
        try {
            const ViewerAction recovery = controller_.undo();
            appendAction(recovery);
            if (!recovery.ok) {
                failProjectRemoval(code + "_recovery_required",
                    message + "; automatic rollback was rejected");
                lockForPublicationRecovery(
                    "Automatic rollback was rejected. Free project storage or "
                    "repair the decision-log destination, then choose Retry recovery.");
                if (show_message) {
                    QMessageBox::critical(this,
                        QStringLiteral("Project recovery required"),
                        QStringLiteral("The geometry change could not be published, "
                                       "and automatic rollback was rejected. Further "
                                       "edits are locked. Repair project storage, then "
                                       "choose Retry recovery."));
                }
                return false;
            }
            publication_recovery_required_ = false;
            removal_completed_success_ = false;
            try {
                restoreRemovalReviewsToPending();
            } catch (const std::exception& error) {
                appendHistory(std::string(
                    "geometry rollback succeeded, but pending-review checkpoint failed: ")
                    + error.what());
            }
            undo_button_->setText(QStringLiteral("Undo"));
            current_entries_ = collect_view_entries(session_);
            display_state_ = DisplayState::before;
            refreshGeometry();
            setDisplayState(DisplayState::before);
            failProjectRemoval(code + "_rolled_back",
                message + "; the live geometry was restored by a journaled undo");
            workspace_status_->setText(QStringLiteral(
                "Publication failed — the geometry was restored. Reopen the "
                "review after resolving project storage."));
            fixed_point_status_->setText(QStringLiteral(
                "The unpublished change was rolled back safely; no further "
                "geometry from that operation is exposed."));
            appendHistory("restored pre-operation geometry after publication failure");
            if (show_message) {
                QMessageBox::warning(this,
                    QStringLiteral("Change not saved"),
                    QStringLiteral("The geometry change passed certification but "
                                   "could not be published to the project. The live "
                                   "model was restored with a journaled undo."));
            }
            return true;
        } catch (const std::exception& error) {
            failProjectRemoval(code + "_recovery_required",
                message + "; rollback journal failed: " + error.what());
            lockForPublicationRecovery(
                std::string("Rollback could not be journaled: ") + error.what()
                + ". Repair project storage, then choose Retry recovery.");
            if (show_message) {
                QMessageBox::critical(this,
                    QStringLiteral("Project recovery required"),
                    QStringLiteral("The change could not be published and rollback "
                                   "could not be journaled:\n\n%1\n\nFurther edits "
                                   "are locked. Repair project storage, then choose "
                                   "Retry recovery.")
                        .arg(QString::fromStdString(error.what())));
            }
            return false;
        }
    }

    bool publishProjectReviewArtifacts(ArtifactRecord* summary_artifact) {
        if (!project_ || artifact_capture_.directory().isEmpty()) return false;
        static const char* names[] = {"removal-plan.json", "events.jsonl",
            "diagnostics.json", "summary.json", "initial-isometric.png",
            "removal-plan.png", "final-certified.png", "rolled-back.png"};
        std::string error;
        bool published_summary = false;
        for (const char* name : names) {
            const std::filesystem::path path =
                artifact_capture_.directory().toStdString() + "/" + name;
            if (!std::filesystem::exists(path)) continue;
            std::string extension = path.extension().string();
            if (!extension.empty() && extension.front() == '.')
                extension.erase(extension.begin());
            ArtifactRecord artifact;
            if (!project_->artifacts().publish_file(
                    "qt-removal-review-v1", extension, path, &artifact, &error)
                || !project_->record_artifact(artifact, &error)) {
                appendHistory("project review artifact failed: " + error);
                return false;
            }
            if (path.filename() == "summary.json") {
                *summary_artifact = artifact;
                published_summary = true;
            }
        }
        return published_summary;
    }

    void finishProjectRemoval(const CommandResult& result) {
        if (!project_ || !project_removal_job_ || project_removal_finalized_)
            return;
        if (!result.ok) {
            ArtifactRecord ignored_summary;
            publishProjectReviewArtifacts(&ignored_summary);
            const std::string code = project_terminal_event_.outcome
                    == JobOutcome::cancelled
                ? "job_cancelled" : "remove_batch_rolled_back";
            failProjectRemoval(code,
                "Qt component-removal transaction did not commit");
            return;
        }
        ArtifactRecord summary_artifact;
        if (!publishProjectReviewArtifacts(&summary_artifact)) {
            recoverFailedProjectRemoval("review_artifact_publish_failed",
                "The certified review packet could not be published");
            return;
        }
        if (project_terminal_event_.phase != JobPhase::finished
            || project_terminal_event_.outcome != JobOutcome::committed
            || !project_removal_progress_->complete(project_terminal_event_,
                summary_artifact, summary_artifact.digest)) {
            const std::string error = project_removal_progress_->last_error();
            recoverFailedProjectRemoval("project_completion_failed", error.empty()
                ? "Project job completion was rejected" : error);
            return;
        }
        project_removal_finalized_ = true;
        appendHistory("completed project job "
            + project_removal_job_->spec.job_id + " with review artifact "
            + summary_artifact.digest);
    }

    void startRemovalPlan() {
        if (removal_running_ || job_controller_->active()) return;
        const std::vector<std::string> targets = selectedRemovalTargets();
        if (targets.empty()) {
            QMessageBox::information(this, QStringLiteral("Empty removal plan"),
                QStringLiteral("Select at least one eligible component or solid."));
            return;
        }
        const std::size_t current_revision =
            document_revision(session_, document_id_);
        if (current_revision != removal_base_revision_) {
            QMessageBox::warning(this, QStringLiteral("Removal plan is stale"),
                QStringLiteral("The document changed from revision %1 to %2. "
                               "Reopen removal review to resolve the targets again.")
                    .arg(removal_base_revision_).arg(current_revision));
            accept_button_->setEnabled(false);
            return;
        }

        double fraction = 0.0;
        std::size_t components = 0;
        std::vector<std::string> component_ids;
        for (int row = 0; row < candidates_->rowCount(); ++row) {
            const QTableWidgetItem* item = candidates_->item(row, 0);
            if (!item || item->checkState() != Qt::Checked) continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index < 0 || index >= static_cast<int>(removal_plan_.size()))
                continue;
            if (removal_plan_[index].protected_target) {
                QMessageBox::critical(this, QStringLiteral("Protected component"),
                    QStringLiteral("The plan contains a protected component and cannot run."));
                return;
            }
            fraction += removal_plan_[index].volume_fraction;
            ++components;
            component_ids.push_back(removal_plan_[index].component_id.empty()
                ? "solid-only:" + removal_plan_[index].target_ids.front()
                : removal_plan_[index].component_id);
        }
        const QString confirmation = QStringLiteral(
            "Remove %1 component(s) containing %2 exact solid target(s)?\n\n"
            "Combined document volume: %3%\nBase revision: %4\n\n"
            "The complete plan is one certified transaction and one undo step.")
            .arg(components).arg(targets.size())
            .arg(number(100.0 * fraction)).arg(removal_base_revision_);
        if (QMessageBox::question(this, QStringLiteral("Confirm component removal"),
                confirmation, QMessageBox::Apply | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Apply) {
            appendHistory("human cancelled component-removal plan");
            return;
        }
        if (!prepareProjectRemovalJob()) return;

        removal_targets_ = targets;
        removal_running_ = true;
        removal_completed_success_ = false;
        removal_close_after_finish_ = decision_callback_ != nullptr;
        pending_terminal_event_ = {};
        artifact_capture_ = {};
        removal_plan_artifact_written_ = false;
        job_dock_->beginJob();
        job_dock_->show();
        for (const std::string& target : targets)
            job_highlights_[target] = JobHighlightRole::removal_candidate;
        viewport_->setJobHighlights(job_highlights_);
        DecisionAuditContext audit = controller_.review().audit;
        if (audit.actor.empty()) audit.actor = "human";
        if (audit.proposal_actor.empty()) audit.proposal_actor = "qt_reviewer";
        if (audit.proposal_id.empty()) audit.proposal_id = "qt-removal-plan";
        audit.approval_mode = "manual";
        std::ostringstream rationale;
        rationale << "human confirmed atomic component removal at revision "
                  << removal_base_revision_ << "; components=";
        for (std::size_t index = 0; index < component_ids.size(); ++index) {
            if (index) rationale << ',';
            rationale << component_ids[index];
        }
        rationale << "; targets=";
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (index) rationale << ',';
            rationale << targets[index];
        }
        if (!audit.rationale.empty())
            audit.rationale += "; " + rationale.str();
        else
            audit.rationale = rationale.str();
        set_decision_audit_context(session_, audit);
        accept_button_->setEnabled(false);
        reject_button_->setEnabled(false);
        undo_button_->setEnabled(false);
        candidates_->setEnabled(false);
        select_removals_button_->setEnabled(false);
        before_button_->setEnabled(false);
        preview_button_->setEnabled(false);
        applied_button_->setEnabled(false);
        run_fixed_point_button_->setEnabled(false);
        review_residual_button_->setEnabled(false);
        cancel_fixed_point_button_->setEnabled(true);
        fixed_point_status_->setText(QStringLiteral(
            "Certifying atomic removal of %1 target(s)…").arg(targets.size()));
        appendHistory("started atomic component-removal plan at revision "
            + std::to_string(removal_base_revision_));
        if (!job_controller_->startBatchRemoval(
                session_, document_id_, targets)) {
            removal_running_ = false;
            clear_decision_audit_context(session_);
            candidates_->setEnabled(true);
            select_removals_button_->setEnabled(true);
            accept_button_->setEnabled(true);
            reject_button_->setEnabled(true);
            undo_button_->setEnabled(true);
            run_fixed_point_button_->setEnabled(true);
            review_residual_button_->setEnabled(true);
            cancel_fixed_point_button_->setEnabled(false);
            appendHistory("could not start component-removal worker");
            failProjectRemoval("qt_worker_start_failed",
                "Could not start the Qt component-removal worker");
        }
    }

    void removalFinished(const CommandResult& result) {
        removal_completed_success_ = result.ok;
        if (result.ok) {
            DecisionAuditContext audit = controller_.review().audit;
            if (audit.actor.empty()) audit.actor = "human";
            audit.approval_mode = "manual";
            for (int row = 0; row < candidates_->rowCount(); ++row) {
                const QTableWidgetItem* item = candidates_->item(row, 0);
                if (!item || item->checkState() != Qt::Checked) continue;
                const int index = item->data(Qt::UserRole).toInt();
                if (index < 0
                    || index >= static_cast<int>(removal_plan_.size())) continue;
                const QtRemovalPlanEntry& entry = removal_plan_[index];
                for (std::size_t target_index = 0;
                     target_index < entry.target_ids.size(); ++target_index) {
                    const std::string proposal_id =
                        target_index < entry.review_ids.size()
                        && !entry.review_ids[target_index].empty()
                        ? entry.review_ids[target_index]
                        : entry.target_ids[target_index];
                    audit.proposal_id = proposal_id;
                    record_decision_review(session_,
                        entry.target_ids[target_index], proposal_id, "accepted",
                        audit, controller_.review().kind, "remove_solid", {},
                        removal_base_revision_);
                }
            }
            current_entries_ = collect_view_entries(session_);
            display_state_ = DisplayState::applied;
            before_button_->setChecked(false);
            preview_button_->setChecked(false);
            applied_button_->setChecked(true);
            refreshDisplayedGeometry();
            viewport_->clearJobHighlights();
            job_highlights_.clear();
            candidates_->setVisible(false);
            select_removals_button_->setVisible(false);
            appendHistory("component-removal plan committed as revision "
                + std::to_string(result.report.document_revision));
        } else {
            display_state_ = DisplayState::before;
            before_button_->setChecked(true);
            refreshDisplayedGeometry();
            appendHistory("component-removal plan stopped without a new revision");
        }
        if (pending_terminal_event_.phase == JobPhase::finished) {
            const QString image = result.ok
                ? QStringLiteral("final-certified.png")
                : QStringLiteral("rolled-back.png");
            artifact_capture_.capture(image,
                [this](const QString& path) { return viewport_->dumpPng(path); });
            artifact_capture_.writeSummary(pending_terminal_event_);
        }
        finishProjectRemoval(result);
        updateDetails();
    }

    void accept() {
        if (job_controller_ && job_controller_->active()) return;
        if (publication_recovery_required_) return;
        if (removalReviewMode()) {
            startRemovalPlan();
            return;
        }
        const DecompositionCandidate* candidate = find_candidate(
            proposal_, controller_.review().candidate_id);
        if (candidate && !candidate->usable) {
            QMessageBox::warning(this, QStringLiteral("Candidate not applicable"),
                QStringLiteral("This proposal did not produce usable geometry. "
                               "Choose another candidate or change the decomposition policy."));
            return;
        }
        QString risk;
        if (candidate
            && (candidate->confidence != "high"
                || candidate->relative_delta > 1.0e-8
                || candidate->pieces_requiring_decomposition > 0
                || candidate->sliver_piece_count > 0)) {
            risk = QStringLiteral(
                "This candidate is marked %1 confidence.\n\n"
                "Relative volume delta: %2\n"
                "Residual pieces: %3\n"
                "Sliver pieces: %4\n\n"
                "Apply it anyway? The decision will be recorded if --log is active.")
                .arg(QString::fromStdString(candidate->confidence),
                     number(candidate->relative_delta))
                .arg(candidate->pieces_requiring_decomposition)
                .arg(candidate->sliver_piece_count);
        } else {
            const ViewerReviewContext& review = controller_.review();
            if (review.has_surface_replacement
                && (!review.surface_replacement.exact
                    || review.surface_replacement.confidence != "high")) {
                const SurfaceReplacementCandidate& surface =
                    review.surface_replacement;
                risk = QStringLiteral(
                    "This is an approximate %1 replacement with %2 confidence.\n\n"
                    "Maximum deviation: %3\n"
                    "Relative volume delta: %4\n\nApply it anyway?")
                    .arg(QString::fromStdString(surface.kind),
                         QString::fromStdString(surface.confidence),
                         number(surface.max_deviation),
                         number(surface.owner_relative_volume_delta));
            } else if (review.has_hole_fill
                       && review.hole_fill.confidence != "high") {
                const HoleFillCandidate& hole = review.hole_fill;
                risk = QStringLiteral(
                    "This hole-fill proposal has %1 confidence.\n\n"
                    "Plug volume: %2\n"
                    "Host volume fraction: %3\n\nApply it anyway?")
                    .arg(QString::fromStdString(hole.confidence),
                         number(hole.plug_volume),
                         number(hole.plug_volume_fraction_of_host));
            }
        }
        if (!risk.isEmpty()
            && QMessageBox::question(this, QStringLiteral("Confirm geometry override"),
                                     risk, QMessageBox::Apply | QMessageBox::Cancel,
                                     QMessageBox::Cancel) != QMessageBox::Apply) {
            appendHistory("human cancelled geometry override");
            return;
        }
        const ViewerAction action = controller_.accept_active();
        if (action.ok) recordActiveReviewOutcome("accepted");
        applyAction(action);
        if (action.ok && decision_callback_) {
            const std::string candidate_id = controller_.review().candidate_id;
            const bool preview_certified = preview_.ok && preview_.summary_matches
                && preview_.candidate_id == candidate_id;
            decision_callback_(true, candidate_id, preview_certified);
            close();
        }
    }

    void reject() {
        if (job_controller_ && job_controller_->active()) return;
        if (publication_recovery_required_) return;
        if (removalReviewMode()) {
            DecisionAuditContext audit = controller_.review().audit;
            if (audit.actor.empty()) audit.actor = "reviewer";
            audit.approval_mode = "manual";
            if (audit.rationale.empty())
                audit.rationale = "reviewer rejected component-removal plan";
            removal_table_updating_ = true;
            std::size_t rejected_targets = 0;
            for (int row = 0; row < candidates_->rowCount(); ++row) {
                QTableWidgetItem* item = candidates_->item(row, 0);
                if (!item || item->checkState() != Qt::Checked) continue;
                const int index = item->data(Qt::UserRole).toInt();
                if (index < 0 || index >= static_cast<int>(removal_plan_.size()))
                    continue;
                const QtRemovalPlanEntry& entry = removal_plan_[index];
                for (std::size_t target_index = 0;
                     target_index < entry.target_ids.size(); ++target_index) {
                    const std::string proposal_id = target_index < entry.review_ids.size()
                        && !entry.review_ids[target_index].empty()
                        ? entry.review_ids[target_index]
                        : (entry.component_id.empty()
                            ? entry.target_ids[target_index] : entry.component_id);
                    audit.proposal_id = proposal_id;
                    record_decision_review(session_, entry.target_ids[target_index],
                        proposal_id, "rejected", audit,
                        controller_.review().kind, "remove_solid", {},
                        removal_base_revision_);
                    ++rejected_targets;
                }
                item->setCheckState(Qt::Unchecked);
            }
            removal_table_updating_ = false;
            updateRemovalPlanPresentation();
            appendHistory("rejected component-removal plan ("
                + std::to_string(rejected_targets) + " target(s)); no change");
            if (decision_callback_) {
                decision_callback_(false, {}, false);
                close();
            }
            return;
        }
        const ViewerAction action = controller_.reject_active();
        applyAction(action);
        if (decision_callback_) {
            const std::string candidate_id = controller_.review().candidate_id;
            const bool preview_certified = preview_.ok && preview_.summary_matches
                && preview_.candidate_id == candidate_id;
            decision_callback_(false, candidate_id, preview_certified);
            close();
        }
    }
    void undo() {
        if (job_controller_ && job_controller_->active()) return;
        if (publication_recovery_required_) {
            recoverFailedProjectRemoval("project_recovery_retry",
                "Retrying rollback after an incomplete project publication");
            return;
        }
        applyAction(controller_.undo());
    }

    bool commit() {
        if (committed_) return true;
        if (commit_path_.empty()) {
            committed_ = true;
            return true;
        }
        if (emission_running_ || job_controller_->active()) return false;

        emission_running_ = true;
        close_after_emission_ = true;
        job_dock_->beginJob();
        job_dock_->show();
        commit_button_->setText(QStringLiteral("Emitting…"));
        commit_button_->setEnabled(false);
        accept_button_->setEnabled(false);
        reject_button_->setEnabled(false);
        undo_button_->setEnabled(false);
        candidates_->setEnabled(false);
        select_removals_button_->setEnabled(false);
        run_fixed_point_button_->setEnabled(false);
        review_residual_button_->setEnabled(false);
        appendHistory("started asynchronous emission -> " + commit_path_);
        if (!job_controller_->startEmission(
                session_, document_id_, commit_path_)) {
            emission_running_ = false;
            close_after_emission_ = false;
            restoreAfterEmission();
            QMessageBox::critical(this, QStringLiteral("Emission unavailable"),
                QStringLiteral("Another geometry task is already active."));
        }
        return false;
    }

    void emissionFinished(const CommandResult& result) {
        emission_running_ = false;
        if (result.ok) {
            committed_ = true;
            appendHistory("verified emission completed -> " + commit_path_);
            commit_button_->setText(QStringLiteral("Emission complete"));
            return;
        }
        close_after_emission_ = false;
        restoreAfterEmission();
        appendHistory("emission failed; the review remains open");
        QMessageBox::critical(this, QStringLiteral("Emission failed"),
            QStringLiteral("The current model could not be emitted. "
                           "Review the job diagnostics and geometry blockers."));
    }

    void restoreAfterEmission() {
        commit_button_->setText(QStringLiteral("Emit and close…"));
        commit_button_->setEnabled(true);
        accept_button_->setEnabled(!removal_completed_success_);
        reject_button_->setEnabled(!removal_completed_success_);
        undo_button_->setEnabled(true);
        candidates_->setEnabled(!removal_completed_success_);
        select_removals_button_->setEnabled(!removal_completed_success_);
        const bool decomposition = controller_.review().kind == "decomposition";
        run_fixed_point_button_->setEnabled(decomposition);
        review_residual_button_->setEnabled(decomposition);
    }

    Session& session_;
    std::string step_path_;
    std::string document_id_;
    const DecompositionProposal* proposal_ = nullptr;
    std::string commit_path_;
    ViewerController controller_;
    std::string selected_id_;
    OcctViewWidget* viewport_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLineEdit* model_search_ = nullptr;
    QLabel* search_status_ = nullptr;
    QLabel* workspace_status_ = nullptr;
    QVBoxLayout* viewport_layout_ = nullptr;
    QTabWidget* inspector_tabs_ = nullptr;
    QTableWidget* candidates_ = nullptr;
    QRadioButton* before_button_ = nullptr;
    QRadioButton* preview_button_ = nullptr;
    QRadioButton* applied_button_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* summary_ = nullptr;
    QTextBrowser* details_ = nullptr;
    QPlainTextEdit* history_ = nullptr;
    QPushButton* accept_button_ = nullptr;
    QPushButton* reject_button_ = nullptr;
    QPushButton* undo_button_ = nullptr;
    QPushButton* run_fixed_point_button_ = nullptr;
    QPushButton* cancel_fixed_point_button_ = nullptr;
    QPushButton* review_residual_button_ = nullptr;
    QPushButton* select_removals_button_ = nullptr;
    QPushButton* commit_button_ = nullptr;
    QShortcut* accept_shortcut_ = nullptr;
    QProgressBar* fixed_point_progress_ = nullptr;
    QLabel* fixed_point_status_ = nullptr;
    DisplayState display_state_ = DisplayState::before;
    std::vector<SolidViewEntry> before_entries_;
    std::vector<SolidViewEntry> current_entries_;
    DecompositionPreview preview_;
    std::function<void(bool, const std::string&, bool)> decision_callback_;
    CadJobController* job_controller_ = nullptr;
    JobProgressDock* job_dock_ = nullptr;
    QTimer* progress_visual_timer_ = nullptr;
    std::optional<JobProgressEvent> pending_visual_progress_;
    ViewArtifactCapture artifact_capture_;
    JobProgressEvent pending_terminal_event_;
    JobProgressEvent project_terminal_event_;
    ProjectStore* project_ = nullptr;
    std::optional<ClaimedProjectJob> project_removal_job_;
    std::unique_ptr<ProjectJobProgressSink> project_removal_progress_;
    ArtifactRecord project_removal_plan_artifact_;
    std::unordered_map<std::string, JobHighlightRole> job_highlights_;
    std::vector<QtRemovalPlanEntry> removal_plan_;
    std::vector<std::string> removal_targets_;
    std::string active_job_target_;
    std::size_t removal_base_revision_ = 0;
    QPointer<QThread> residual_review_thread_;
    DecompositionProposal generated_proposal_;
    bool fixed_point_running_ = false;
    bool preview_running_ = false;
    bool residual_review_running_ = false;
    bool removal_table_updating_ = false;
    bool removal_table_connected_ = false;
    bool removal_running_ = false;
    bool emission_running_ = false;
    bool close_after_emission_ = false;
    bool removal_completed_success_ = false;
    bool removal_close_after_finish_ = false;
    bool removal_plan_artifact_written_ = false;
    bool project_removal_finalized_ = false;
    bool project_plan_event_link_pending_ = false;
    bool publication_recovery_required_ = false;
    bool candidates_connected_ = false;
    bool committed_ = false;
    bool focused_initial_display_ = false;
    bool inspection_has_mutation_ = false;
};

bool edit_medium_request(QWidget* parent,
                         Session& session,
                         const DocumentId& document_id,
                         const AutonomyPolicy& policy,
                         MediumGenerationRequest* request,
                         MediumGenerationProposal* certified) {
    if (!request || !certified) return false;
    const ConversionReport report = analyze(
        session, document_id, ReportIdMode::local).report;
    struct HostChoice {
        std::string instance_id;
        std::string label;
        PipeAnnulusTopology pipe;
    };
    std::vector<HostChoice> hosts;
    for (const ComponentInstance& instance : report.component_instances) {
        const auto solid = std::find_if(
            report.solids.begin(), report.solids.end(),
            [&](const AnalyzedSolid& value) {
                return value.id == instance.source_shape_id;
            });
        if (solid == report.solids.end()
            || !solid->topology.pipe_annulus.candidate) continue;
        hosts.push_back({instance.id,
            solid->name.empty() ? instance.id : solid->name,
            solid->topology.pipe_annulus});
    }

    std::vector<MaterialDefinition> materials = report.materials;
    for (const MaterialDefinition& configured
         : policy.contained_medium_materials) {
        if (std::none_of(materials.begin(), materials.end(),
                [&](const MaterialDefinition& value) {
                    return value.id == configured.id;
                })) {
            materials.push_back(configured);
        }
    }

    // Network requests are normally authored by the project policy/agent
    // because reciprocal occurrence-qualified peer IDs are cumbersome to
    // recreate safely in a two-column widget. Preserve the typed request and
    // provide a certificate gate instead of silently collapsing it to the
    // first host (the v1 editor's former behaviour).
    if (request->host_component_ids.size() > 1) {
        *certified = propose_contained_medium(
            session, document_id, *request, ReportIdMode::local);
        const QString summary = certified->ok
            ? QStringLiteral(
                "Connected network certificate passed.\n\n"
                "Segments: %1\nConnections: %2\nCircuits: %3\n"
                "Generated volume: %4\nLiquid: %5\nGas: %6\n"
                "The 3D review will show every lumen segment before commit.")
                .arg(certified->certificate.network_segment_count)
                .arg(certified->certificate.network_connection_count)
                .arg(certified->certificate.network_connected_component_count)
                .arg(number(certified->certificate.generated_volume))
                .arg(number(certified->certificate.liquid_volume))
                .arg(number(certified->certificate.gas_volume))
            : QStringLiteral("Connected network is blocked: %1")
                .arg(QString::fromStdString(certified->diagnostics.empty()
                    ? "certificate_failed"
                    : certified->diagnostics.front().code));
        if (!certified->ok) {
            QMessageBox::critical(parent,
                QStringLiteral("Contained-medium network blocked"), summary);
            return false;
        }
        return QMessageBox::question(parent,
            QStringLiteral("Review contained-medium network"), summary,
            QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Contained-medium request"));
    dialog.resize(620, 430);
    auto* layout = new QVBoxLayout(&dialog);
    auto* introduction = new QLabel(
        QStringLiteral("Select the physical fluid, host pipe, and treatment "
                       "of every lumen boundary. Apply remains disabled until "
                       "a fresh OCCT certificate passes."), &dialog);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);
    auto* form = new QFormLayout();
    auto* material_combo = new QComboBox(&dialog);
    for (const MaterialDefinition& material : materials) {
        if (material.phase != MaterialPhase::liquid
            && material.phase != MaterialPhase::gas) continue;
        material_combo->addItem(
            QString::fromStdString(material.id + " ("
                + material_phase_name(material.phase) + ")"),
            QString::fromStdString(material.id));
    }
    auto* host_combo = new QComboBox(&dialog);
    for (const HostChoice& host : hosts) {
        host_combo->addItem(QString::fromStdString(
            host.label + " — " + host.instance_id),
            QString::fromStdString(host.instance_id));
    }
    form->addRow(QStringLiteral("Material"), material_combo);
    form->addRow(QStringLiteral("Host pipe"), host_combo);
    layout->addLayout(form);
    auto* ports = new QTableWidget(&dialog);
    ports->setColumnCount(2);
    ports->setHorizontalHeaderLabels(
        {QStringLiteral("Port"), QStringLiteral("Boundary treatment")});
    ports->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ports->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(ports, 1);
    auto* certificate_label = new QLabel(&dialog);
    certificate_label->setWordWrap(true);
    layout->addWidget(certificate_label);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(
        QStringLiteral("Review certified geometry"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);

    const auto select_data = [](QComboBox* combo, const std::string& value) {
        const int index = combo->findData(QString::fromStdString(value));
        if (index >= 0) combo->setCurrentIndex(index);
    };
    select_data(material_combo, request->material_id);
    if (!request->host_component_ids.empty())
        select_data(host_combo, request->host_component_ids.front());

    std::function<void()> refresh;
    std::function<void()> rebuild_ports;
    rebuild_ports = [&]() {
        std::map<std::string, MediumPortDisposition> previous;
        for (const MediumPortDecision& port : request->ports)
            previous[port.port_id] = port.disposition;
        ports->setRowCount(0);
        const int host_index = host_combo->currentIndex();
        if (host_index < 0 || host_index >= static_cast<int>(hosts.size()))
            return;
        const auto& recognized_ports = hosts[host_index].pipe.ports;
        ports->setRowCount(static_cast<int>(recognized_ports.size()));
        for (int row = 0; row < static_cast<int>(recognized_ports.size()); ++row) {
            const PipePortTopology& port = recognized_ports[row];
            auto* id = new QTableWidgetItem(QString::fromStdString(port.id));
            id->setFlags(id->flags() & ~Qt::ItemIsEditable);
            ports->setItem(row, 0, id);
            auto* disposition = new QComboBox(ports);
            const MediumPortDisposition values[] = {
                MediumPortDisposition::unresolved,
                MediumPortDisposition::cap_here,
                MediumPortDisposition::interface_to_external_model,
                MediumPortDisposition::connect_to_port,
                MediumPortDisposition::open_to_domain,
            };
            for (const MediumPortDisposition value : values) {
                disposition->addItem(QString::fromUtf8(
                    medium_port_disposition_name(value)),
                    static_cast<int>(value));
            }
            const auto found = previous.find(port.id);
            if (found != previous.end()) {
                disposition->setCurrentIndex(disposition->findData(
                    static_cast<int>(found->second)));
            }
            ports->setCellWidget(row, 1, disposition);
            QObject::connect(disposition,
                qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                [&](int) { if (refresh) refresh(); });
        }
    };
    refresh = [&]() {
        request->material_id = material_combo->currentData().toString().toStdString();
        request->host_component_ids.clear();
        if (host_combo->currentIndex() >= 0) {
            request->host_component_ids.push_back(
                host_combo->currentData().toString().toStdString());
        }
        request->fill_mode = MediumFillMode::full;
        request->ports.clear();
        for (int row = 0; row < ports->rowCount(); ++row) {
            auto* disposition = qobject_cast<QComboBox*>(ports->cellWidget(row, 1));
            if (!disposition || !ports->item(row, 0)) continue;
            request->ports.push_back({
                ports->item(row, 0)->text().toStdString(),
                static_cast<MediumPortDisposition>(
                    disposition->currentData().toInt()), {}, 0});
        }
        *certified = propose_contained_medium(
            session, document_id, *request, ReportIdMode::local);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(certified->ok);
        if (certified->ok) {
            certificate_label->setText(QStringLiteral(
                "Certificate passed: valid, closed, leak-free, and disjoint. "
                "Generated volume: %1.")
                .arg(number(certified->certificate.generated_volume)));
        } else {
            const std::string code = certified->diagnostics.empty()
                ? "incomplete_request" : certified->diagnostics.front().code;
            certificate_label->setText(QStringLiteral(
                "Blocked: %1. Resolve every port or correct the selected host/material.")
                .arg(QString::fromStdString(code)));
        }
    };
    QObject::connect(material_combo,
        qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [&](int) { refresh(); });
    QObject::connect(host_combo,
        qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [&](int) { rebuild_ports(); refresh(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    rebuild_ports();
    refresh();
    return dialog.exec() == QDialog::Accepted && certified->ok;
}

class CoverageReviewWindow final : public QMainWindow {
public:
    CoverageReviewWindow(AleaCoverageCertificate certificate,
                         std::string certificate_path)
        : certificate_(std::move(certificate)),
          items_(alea_coverage_review_items(certificate_)) {
        setWindowTitle(QStringLiteral("csgify coverage review — ")
                       + QString::fromStdString(certificate_path));
        resize(1120, 720);
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        const bool certified = certificate_.status
            == AleaCoverageCertificateStatus::certified;
        summary_ = new QLabel(QStringLiteral(
            "Domain: %1 · Status: %2\n"
            "Material cells: %3 · Void cells: %4 · Graveyard cells: %5 · "
            "Exact checks: %6")
            .arg(QString::fromStdString(certificate_.domain.domain_id),
                 QString::fromLatin1(
                     alea_coverage_status_name(certificate_.status)))
            .arg(certificate_.material_cell_count)
            .arg(certificate_.intentional_void_cell_count)
            .arg(certificate_.graveyard_cell_count)
            .arg(certificate_.exact_checks_performed), central);
        summary_->setWordWrap(true);
        QPalette palette = summary_->palette();
        palette.setColor(QPalette::WindowText,
            certified ? QColor(35, 145, 65) : QColor(190, 65, 45));
        summary_->setPalette(palette);
        layout->addWidget(summary_);

        auto* filter_row = new QHBoxLayout();
        search_ = new QLineEdit(central);
        search_->setPlaceholderText(
            QStringLiteral("Search finding, category, or recommended action"));
        filter_ = new QComboBox(central);
        filter_->addItem(QStringLiteral("All findings"), QStringLiteral("all"));
        filter_->addItem(QStringLiteral("Blocking only"), QStringLiteral("blocking"));
        filter_->addItem(QStringLiteral("Information only"), QStringLiteral("info"));
        filter_row->addWidget(search_, 1);
        filter_row->addWidget(filter_);
        layout->addLayout(filter_row);

        table_ = new QTableWidget(central);
        table_->setColumnCount(5);
        table_->setHorizontalHeaderLabels({QStringLiteral("Finding"),
            QStringLiteral("Category"), QStringLiteral("State"),
            QStringLiteral("Count"), QStringLiteral("Recommended action")});
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->horizontalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_, 1);

        details_ = new QTextBrowser(central);
        details_->setMinimumHeight(190);
        layout->addWidget(details_);
        auto* footer = new QHBoxLayout();
        policy_ = new QLabel(certified
            ? QStringLiteral("No authorization is required: this certificate has no blocking ownership finding.")
            : QStringLiteral("Unsafe geometry is never auto-approved. A user or authorized LLM must choose and execute a corrective operation, then regenerate the certificate."),
            central);
        policy_->setWordWrap(true);
        auto* close = new QPushButton(QStringLiteral("Close review"), central);
        footer->addWidget(policy_, 1);
        footer->addWidget(close);
        layout->addLayout(footer);
        setCentralWidget(central);

        connect(search_, &QLineEdit::textChanged, this,
                [this]() { refreshTable(); });
        connect(filter_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { refreshTable(); });
        connect(table_, &QTableWidget::itemSelectionChanged, this,
                [this]() { updateDetails(); });
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        refreshTable();
    }

private:
    void refreshTable() {
        const QString needle = search_->text().trimmed();
        const QString mode = filter_->currentData().toString();
        visible_.clear();
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const AleaCoverageReviewItem& item = items_[i];
            if (mode == QStringLiteral("blocking") && !item.blocking) continue;
            if (mode == QStringLiteral("info") && item.blocking) continue;
            const QString searchable = QString::fromStdString(
                item.id + " " + item.category + " " + item.summary + " "
                + item.recommended_action);
            if (!needle.isEmpty()
                && !searchable.contains(needle, Qt::CaseInsensitive)) continue;
            visible_.push_back(i);
        }
        table_->setRowCount(static_cast<int>(visible_.size()));
        for (int row = 0; row < static_cast<int>(visible_.size()); ++row) {
            const AleaCoverageReviewItem& item = items_[visible_[row]];
            const QString values[] = {QString::fromStdString(item.id),
                QString::fromStdString(item.category),
                item.blocking ? QStringLiteral("BLOCKING")
                              : QStringLiteral("information"),
                QString::number(static_cast<qulonglong>(item.count)),
                QString::fromStdString(item.recommended_action)};
            for (int column = 0; column < 5; ++column) {
                auto* cell = new QTableWidgetItem(values[column]);
                if (item.blocking) cell->setForeground(QColor(190, 65, 45));
                cell->setData(Qt::UserRole,
                              static_cast<qulonglong>(visible_[row]));
                table_->setItem(row, column, cell);
            }
        }
        if (!visible_.empty()) table_->selectRow(0);
        else details_->setHtml(QStringLiteral("No findings match the current filter."));
    }

    void updateDetails() {
        const int row = table_->currentRow();
        if (row < 0 || row >= static_cast<int>(visible_.size())) return;
        const AleaCoverageReviewItem& item = items_[visible_[row]];
        QString html = QStringLiteral(
            "<h3>%1</h3><b>%2</b> · count %3<br><p>%4</p>"
            "<hr><b>Recommended next action</b><p>%5</p>"
            "<b>Autonomous mutation:</b> %6")
            .arg(QString::fromStdString(item.id),
                 item.blocking ? QStringLiteral("Blocking finding")
                               : QStringLiteral("Information"))
            .arg(static_cast<qulonglong>(item.count))
            .arg(QString::fromStdString(item.summary),
                 QString::fromStdString(item.recommended_action),
                 item.auto_action_allowed ? QStringLiteral("allowed by policy")
                                          : QStringLiteral("not authorized"));
        html += QStringLiteral(
            "<hr><b>Certificate evidence</b><br>"
            "Structural exact complement: %1<br>Committed: %2; merged: %3<br>"
            "Coverage samples: %4; unique: %5; gaps: %6; overlaps: %7; "
            "undefined fills: %8; unresolved: %9; truncated: %10<br>"
            "Workers: %11; reserved scratch: %12 bytes<br>"
            "Fill instances: %13; reused universes: %14; duplicated cells avoided: %15")
            .arg(certificate_.structural_exact_complement ? "yes" : "no")
            .arg(certificate_.committed ? "yes" : "no")
            .arg(certificate_.merged ? "yes" : "no")
            .arg(certificate_.coverage.sample_count)
            .arg(certificate_.coverage.unique_count)
            .arg(certificate_.coverage.gap_count)
            .arg(certificate_.coverage.overlap_count)
            .arg(certificate_.coverage.undefined_fill_count)
            .arg(certificate_.coverage.unresolved_count)
            .arg(certificate_.coverage.truncated_count)
            .arg(certificate_.execution.actual_workers)
            .arg(certificate_.execution.reserved_parallel_scratch_bytes)
            .arg(certificate_.fill_instance_count)
            .arg(certificate_.reused_universe_count)
            .arg(certificate_.estimated_duplicated_cells_avoided);
        if (!certificate_.limitations.empty()) {
            html += QStringLiteral("<hr><b>Limitations</b><ul>");
            for (const std::string& limitation : certificate_.limitations)
                html += QStringLiteral("<li>%1</li>")
                    .arg(QString::fromStdString(limitation).toHtmlEscaped());
            html += QStringLiteral("</ul>");
        }
        details_->setHtml(html);
    }

    AleaCoverageCertificate certificate_;
    std::vector<AleaCoverageReviewItem> items_;
    std::vector<std::size_t> visible_;
    QLabel* summary_ = nullptr;
    QLabel* policy_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* filter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTextBrowser* details_ = nullptr;
};

class AutonomyQueueWindow final : public QMainWindow {
public:
    AutonomyQueueWindow(Session& session,
                        std::string step_path,
                        AutonomyQueue& queue,
                        std::string queue_path,
                        ProjectStore* project = nullptr)
        : session_(session), step_path_(std::move(step_path)), queue_(queue),
          queue_path_(std::move(queue_path)), project_(project) {
        setWindowTitle(QStringLiteral("csgify autonomy queue — ")
                       + QString::fromStdString(step_path_));
        resize(1250, 650);
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        authority_ = new QLabel(central);
        authority_->setObjectName(QStringLiteral("agentAuthoritySummary"));
        authority_->setWordWrap(true);
        authority_->setAccessibleName(QStringLiteral("Agent authority and mode"));
        authority_->setStyleSheet(QStringLiteral(
            "padding:7px;border:1px solid #6d7682;border-radius:3px;"));
        layout->addWidget(authority_);
        summary_ = new QLabel(central);
        summary_->setObjectName(QStringLiteral("agentQueueSummary"));
        summary_->setAccessibleName(QStringLiteral("Agent proposal queue summary"));
        layout->addWidget(summary_);
        table_ = new QTableWidget(central);
        table_->setObjectName(QStringLiteral("agentProposalTable"));
        table_->setAccessibleName(QStringLiteral("Typed agent proposals"));
        table_->setColumnCount(7);
        table_->setHorizontalHeaderLabels({QStringLiteral("ID"),
            QStringLiteral("Operation"), QStringLiteral("Target"),
            QStringLiteral("State"), QStringLiteral("Confidence"),
            QStringLiteral("Policy rule"), QStringLiteral("Reason")});
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table_, 1);
        explanation_ = new QTextBrowser(central);
        explanation_->setObjectName(QStringLiteral("agentProposalExplanation"));
        explanation_->setAccessibleName(QStringLiteral(
            "Selected agent proposal, engine evidence, and policy result"));
        explanation_->setMinimumHeight(210);
        layout->addWidget(explanation_);
        auto* buttons = new QHBoxLayout();
        review_button_ = new QPushButton(QStringLiteral("Review selected"), central);
        review_button_->setObjectName(QStringLiteral("reviewAgentProposalButton"));
        apply_certified_button_ = new QPushButton(
            QStringLiteral("Evaluate certified changes…"), central);
        apply_certified_button_->setObjectName(
            QStringLiteral("applyCertifiedAgentChangesButton"));
        refresh_button_ = new QPushButton(
            QStringLiteral("Re-evaluate evidence"), central);
        refresh_button_->setObjectName(QStringLiteral("reevaluateAgentEvidenceButton"));
        auto* close = new QPushButton(QStringLiteral("Close"), central);
        buttons->addWidget(review_button_);
        buttons->addWidget(apply_certified_button_);
        buttons->addWidget(refresh_button_);
        buttons->addStretch(1);
        buttons->addWidget(close);
        layout->addLayout(buttons);
        setCentralWidget(central);
        connect(review_button_, &QPushButton::clicked, this,
                [this]() { reviewSelected(); });
        connect(table_, &QTableWidget::itemDoubleClicked, this,
                [this](QTableWidgetItem*) { reviewSelected(); });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
            updateSelectedProposal();
        });
        connect(apply_certified_button_, &QPushButton::clicked,
                this, [this]() { applyCertified(); });
        connect(refresh_button_, &QPushButton::clicked, this,
                [this]() { startReevaluation(); });
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        job_controller_ = new CadJobController(this);
        connect(job_controller_, &CadJobController::autonomyOperationCompleted,
                this, [this](const AutonomyOperationResultPtr& result) {
                    autonomyOperationFinished(result);
                });
        connect(job_controller_, &CadJobController::failed, this,
                [this](const QString&, const QString& message) {
                    operation_running_ = false;
                    active_operation_id_.clear();
                    refreshTable();
                    QMessageBox::critical(this,
                        QStringLiteral("Geometry operation failed"), message);
                });
        connect(job_controller_, &CadJobController::activeChanged, this,
                [this](bool active) {
                    if (!active && pending_reevaluation_) {
                        pending_reevaluation_ = false;
                        QTimer::singleShot(0, this,
                            [this]() { startReevaluation(); });
                    }
                });
        refreshTable();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (reevaluating_ || operation_running_) {
            summary_->setText(QStringLiteral(
                "The active geometry job is finishing its current OCCT operation. "
                "The queue remains unchanged until the worker completes."));
            event->ignore();
            return;
        }
        persist();
        event->accept();
    }

private:
    std::size_t currentRevision() const {
        return document_revision(session_, queue_.document_id);
    }

    bool isStale(const AutonomyQueueItem& item) const {
        return !item.executed && !item.rejected
            && item.evaluated_revision != currentRevision();
    }

    static bool transactionalReviewOperation(const std::string& operation) {
        return operation == "decompose_to_fixed_point"
            || operation == "shatter_with_approximation"
            || operation == "remove_residual_with_approximation"
            || operation == "resolve_overlap_with_approximation";
    }

    bool policyAllowsTransactionalReview(
        const AutonomyQueueItem& item, QString* blocker) const {
        const ApproximationPolicy& approximation = queue_.policy.approximation;
        QString reason;
        if (queue_.policy.mode == AutonomyMode::explain) {
            reason = QStringLiteral(
                "Advisory mode cannot run geometry mutations. Change project "
                "authority to Assisted or Delegated, reload, and re-evaluate.");
        } else if (item.proposal.op == "decompose_to_fixed_point") {
            // The coordinator commits certified actions only and stops when
            // further progress needs engineering judgment.
        } else if (item.proposal.op == "shatter_with_approximation"
                   && (!approximation.enabled
                       || !approximation.allow_numerical_shatter_loss)) {
            reason = QStringLiteral(
                "Budgeted shatter is disabled. Enable numerical shatter loss "
                "in the project approximation policy, reload, and re-evaluate.");
        } else if (item.proposal.op
                       == "remove_residual_with_approximation"
                   && (!approximation.enabled
                       || !approximation.allow_sliver_removal
                       || approximation.require_topology_preservation)) {
            reason = QStringLiteral(
                "Residual removal requires an approximation budget, explicit "
                "residual-removal authority, and topology preservation disabled. "
                "Edit project policy, reload, and re-evaluate.");
        } else if (item.proposal.op
                       == "resolve_overlap_with_approximation"
                   && (!approximation.enabled
                       || !approximation.allow_overlap_resolution
                       || item.proposal.candidate_id.empty())) {
            reason = QStringLiteral(
                "Overlap ownership requires an approximation budget, explicit "
                "overlap-resolution authority, and a proposed owner. Edit the "
                "policy or proposal, then re-evaluate.");
        }
        if (blocker) *blocker = reason;
        return reason.isEmpty();
    }

    QString transactionalReviewText(const AutonomyQueueItem& item) const {
        const ApproximationPolicy& policy = queue_.policy.approximation;
        const ApproximationLedger ledger = document_approximation_ledger(
            session_, queue_.document_id);
        const double used = ledger.document_baseline_volume > 0.0
            ? ledger.cumulative_changed_volume / ledger.document_baseline_volume
            : 0.0;
        QString change;
        if (item.proposal.op == "decompose_to_fixed_point") {
            change = QStringLiteral(
                "Run the bounded fixed-point coordinator. It applies only "
                "certified decomposition progress and leaves uncertain residuals intact.");
        } else if (item.proposal.op == "shatter_with_approximation") {
            change = QStringLiteral(
                "Attempt bounded shatter to depth %1. Numerical loss is measured "
                "and the mutation rolls back if a budget or overlap gate fails.")
                .arg(item.proposal.parameters.shatter_max_depth);
        } else if (item.proposal.op
                   == "remove_residual_with_approximation") {
            change = QStringLiteral(
                "Remove residual %1 only if measured changed and removed volume "
                "remain inside the configured budgets.")
                .arg(QString::fromStdString(item.proposal.target_id));
        } else {
            change = QStringLiteral(
                "Assign the overlap involving %1 to owner %2. The engine measures "
                "the subtraction and rolls back unless budget and overlap safety pass.")
                .arg(QString::fromStdString(item.proposal.target_id),
                     QString::fromStdString(item.proposal.candidate_id));
        }
        return QStringLiteral(
            "%1\n\nGlobal changed-volume budget: %2%\nUsed before this action: %3%\n"
            "Per-body budget: %4%\nPer-action budget: %5%\n\n"
            "Approval authorizes a certified attempt, not an unchecked override.")
            .arg(change)
            .arg(number(100.0 * policy.max_global_changed_volume_fraction),
                 number(100.0 * used),
                 number(100.0 * policy.max_body_changed_volume_fraction),
                 number(100.0 * policy.max_action_changed_volume_fraction));
    }

    void reviewTransactionalOperation(AutonomyQueueItem& item) {
        QString blocker;
        if (!policyAllowsTransactionalReview(item, &blocker)) {
            QMessageBox::information(this,
                QStringLiteral("Project policy change required"), blocker);
            return;
        }
        if (QMessageBox::question(this,
                QStringLiteral("Confirm certified geometry attempt"),
                transactionalReviewText(item),
                QMessageBox::Apply | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Apply) {
            return;
        }
        operation_running_ = true;
        active_operation_id_ = item.proposal.id;
        table_->setEnabled(false);
        review_button_->setEnabled(false);
        apply_certified_button_->setEnabled(false);
        refresh_button_->setEnabled(false);
        summary_->setText(QStringLiteral(
            "Running the approved operation on a geometry worker. The engine "
            "will commit only certified results inside project policy."));
        if (!job_controller_->startAutonomyOperation(
                session_, queue_.document_id, item, queue_.policy)) {
            operation_running_ = false;
            active_operation_id_.clear();
            refreshTable();
            QMessageBox::warning(this, QStringLiteral("Geometry worker busy"),
                QStringLiteral("Another geometry writer is active. Retry when it finishes."));
        }
    }

    void autonomyOperationFinished(const AutonomyOperationResultPtr& result) {
        operation_running_ = false;
        table_->setEnabled(true);
        apply_certified_button_->setEnabled(true);
        refresh_button_->setEnabled(true);
        if (result) {
            for (AutonomyQueueItem& current : queue_.items) {
                if (current.proposal.id != active_operation_id_) continue;
                current = result->item;
                break;
            }
        }
        active_operation_id_.clear();
        persist();
        refreshTable();
        pending_reevaluation_ = result && result->execution.ok;
    }

    QString authorityText() const {
        if (queue_.mode == AutonomyMode::explain) {
            return QStringLiteral(
                "Advisory mode · The agent may explain evidence but cannot "
                "apply geometry changes.");
        }
        if (queue_.mode == AutonomyMode::automatic) {
            return QStringLiteral(
                "Delegated mode · Only operations allowed by the immutable "
                "policy may be auto-applied after fresh engine certification. "
                "The agent cannot expand this authority.");
        }
        return QStringLiteral(
            "Assisted mode · The agent may propose and explain. Every geometry "
            "mutation requires an explicit human decision.");
    }

    QString stateText(const AutonomyQueueItem& item) const {
        if (item.rejected) return QStringLiteral("rejected");
        if (item.executed && item.execution_ok
            && item.proposal.op.rfind("coverage_", 0) == 0) {
            return QStringLiteral("verified resolved");
        }
        if (item.executed) return item.execution_ok
            ? QStringLiteral("applied") : QStringLiteral("failed");
        if (isStale(item)) return QStringLiteral("stale — re-evaluate");
        return QString::fromLatin1(autonomy_disposition_name(item.disposition));
    }

    void updateSelectedProposal() {
        const int row = table_->currentRow();
        if (row < 0 || row >= static_cast<int>(queue_.items.size())) {
            explanation_->setHtml(QStringLiteral(
                "<h3>No proposal selected</h3>Select an item to compare the "
                "agent explanation with authoritative engine evidence and policy."));
            review_button_->setEnabled(false);
            return;
        }
        const AutonomyQueueItem& item = queue_.items[row];
        const bool stale = isStale(item);
        const std::size_t revision = currentRevision();
        const QString actor = QString::fromStdString(item.proposal.actor).toHtmlEscaped();
        const QString proposal_id = QString::fromStdString(item.proposal.id).toHtmlEscaped();
        const QString operation = QString::fromStdString(item.proposal.op).toHtmlEscaped();
        const QString target = QString::fromStdString(item.proposal.target_id).toHtmlEscaped();
        const QString candidate = item.proposal.candidate_id.empty()
            ? QStringLiteral("—")
            : QString::fromStdString(item.proposal.candidate_id).toHtmlEscaped();
        const QString rationale = item.proposal.rationale.empty()
            ? QStringLiteral("No explanation supplied.")
            : QString::fromStdString(item.proposal.rationale).toHtmlEscaped();
        const QString rule = QString::fromStdString(item.policy_rule).toHtmlEscaped();
        const QString reason = QString::fromStdString(item.reason).toHtmlEscaped();
        const auto yes_no = [](bool value) {
            return value ? QStringLiteral("yes") : QStringLiteral("no");
        };

        QString html;
        if (stale) {
            html += QStringLiteral(
                "<div style='border:2px solid #d27b22;padding:7px'>"
                "<b>STALE — review disabled</b><br>Evidence was evaluated at "
                "revision %1, but the document is now revision %2. Re-evaluate "
                "before previewing or approving this proposal.</div>")
                .arg(item.evaluated_revision).arg(revision);
        }
        html += QStringLiteral(
            "<h3>Agent explanation <small>(interpretation, not evidence)</small></h3>"
            "Proposal: %1<br>Actor: %2<br>Rationale: %3"
            "<h3>Requested action</h3>Operation: %4<br>Target: %5<br>Candidate: %6"
            "<h3>Engine evidence <small>(authoritative and regenerated)</small></h3>"
            "Evaluated revision: %7 · Current revision: %8<br>"
            "Usable: %9 · Exact: %10 · Certificate passed: %11<br>"
            "Confidence: %12 · Pieces: %13 · Residuals: %14 · Slivers: %15<br>"
            "Relative volume delta: %16 · Maximum deviation: %17 · Volume fraction: %18"
            "<h3>Policy result</h3>State: %19<br>Rule: %20<br>Reason: %21")
            .arg(proposal_id, actor, rationale, operation, target, candidate)
            .arg(item.evaluated_revision).arg(revision)
            .arg(yes_no(item.evidence.usable), yes_no(item.evidence.exact),
                 yes_no(item.evidence.certificate_passed))
            .arg(QString::fromStdString(item.evidence.confidence).toHtmlEscaped())
            .arg(item.evidence.piece_count).arg(item.evidence.residual_count)
            .arg(item.evidence.sliver_count)
            .arg(number(item.evidence.relative_volume_delta),
                 number(item.evidence.max_deviation),
                 number(item.evidence.volume_fraction), stateText(item), rule,
                 reason);
        explanation_->setHtml(html);

        const bool reviewable = !stale && !item.executed && !item.rejected
            && item.disposition == AutonomyDisposition::human_review;
        const bool transactional = transactionalReviewOperation(
            item.proposal.op);
        QString blocker;
        const bool runnable = !transactional
            || policyAllowsTransactionalReview(item, &blocker);
        review_button_->setText(transactional
            ? (runnable ? QStringLiteral("Review and run selected…")
                        : QStringLiteral("View recovery steps…"))
            : QStringLiteral("Review selected"));
        review_button_->setEnabled(reviewable && !preparing_review_
                                   && !operation_running_);
        review_button_->setToolTip(stale
            ? QStringLiteral("Evidence is stale; re-evaluate before review.")
            : (reviewable && transactional
                   ? (runnable
                       ? QStringLiteral("Review policy budgets before a certified attempt.")
                       : blocker)
                   : (reviewable
                       ? QStringLiteral("Open the ordinary geometry preview and decision flow.")
                       : QStringLiteral("This proposal is not awaiting human review."))));
    }

    void refreshTable() {
        refresh_autonomy_queue_counts(queue_);
        authority_->setText(authorityText());
        if (apply_certified_button_) {
            apply_certified_button_->setText(queue_.auto_apply_count > 0
                ? QStringLiteral("Apply %1 certified %2…")
                    .arg(queue_.auto_apply_count)
                    .arg(queue_.auto_apply_count == 1
                        ? QStringLiteral("change") : QStringLiteral("changes"))
                : QStringLiteral("Evaluate certified changes…"));
        }
        QString summary = QStringLiteral(
            "Applied %1 · Human review %2 · Denied %3 · Failed %4")
            .arg(queue_.applied_count).arg(queue_.human_review_count)
            .arg(queue_.denied_count).arg(queue_.failed_count);
        if (queue_.policy.approximation.enabled) {
            const ApproximationLedger ledger = document_approximation_ledger(
                session_, queue_.document_id);
            const double used = ledger.document_baseline_volume > 0.0
                ? ledger.cumulative_changed_volume
                    / ledger.document_baseline_volume : 0.0;
            summary += QStringLiteral(
                "\nApproximation: limit %1% · used %2% · overlap safety remains mandatory")
                .arg(number(100.0 * queue_.policy.approximation
                                      .max_global_changed_volume_fraction),
                     number(100.0 * used));
        } else {
            summary += QStringLiteral("\nApproximation: disabled · exact-only");
        }
        summary_->setText(summary);
        table_->setRowCount(static_cast<int>(queue_.items.size()));
        for (int row = 0; row < static_cast<int>(queue_.items.size()); ++row) {
            const AutonomyQueueItem& item = queue_.items[row];
            const QString values[] = {QString::fromStdString(item.proposal.id),
                QString::fromStdString(item.proposal.op),
                QString::fromStdString(item.proposal.target_id), stateText(item),
                QString::fromStdString(item.evidence.confidence),
                QString::fromStdString(item.policy_rule),
                QString::fromStdString(item.rejected
                    ? item.review_rationale : item.reason)};
            for (int column = 0; column < 7; ++column) {
                auto* cell = new QTableWidgetItem(values[column]);
                cell->setData(Qt::UserRole, row);
                table_->setItem(row, column, cell);
            }
        }
        int selected_row = -1;
        for (int row = 0; row < static_cast<int>(queue_.items.size()); ++row) {
            const AutonomyQueueItem& item = queue_.items[row];
            if (!item.executed && !item.rejected
                && item.disposition == AutonomyDisposition::human_review) {
                selected_row = row;
                break;
            }
        }
        if (selected_row < 0 && !queue_.items.empty()) selected_row = 0;
        if (selected_row >= 0) table_->selectRow(selected_row);
        updateSelectedProposal();
    }

    void persist() {
        if (!queue_path_.empty() && queue_path_ != "-")
            write_autonomy_queue_json(queue_, queue_path_);
    }

    void startReevaluation() {
        if (reevaluating_ || preparing_review_ || operation_running_) return;
        reevaluating_ = true;
        table_->setEnabled(false);
        review_button_->setEnabled(false);
        apply_certified_button_->setEnabled(false);
        refresh_button_->setEnabled(false);
        refresh_button_->setText(QStringLiteral("Re-evaluating…"));
        authority_->setText(QStringLiteral(
            "Waiting for engine · Regenerating proposal evidence against "
            "the current document revision."));
        summary_->setText(QStringLiteral(
            "The interface remains available; review is disabled until fresh "
            "engine evidence returns."));

        auto refreshed = std::make_shared<AutonomyQueue>(queue_);
        auto error = std::make_shared<std::string>();
        Session* session = &session_;
        QPointer<AutonomyQueueWindow> guard(this);
        QThread* thread = QThread::create(
            [session, refreshed, error, guard]() {
                try {
                    reevaluate_pending_autonomy_queue(
                        *session, *refreshed, refreshed->policy);
                } catch (const std::exception& caught) {
                    *error = caught.what();
                } catch (...) {
                    *error = "OCCT rejected autonomy evidence regeneration";
                }
                if (guard) {
                    QMetaObject::invokeMethod(guard,
                        [guard, refreshed, error]() {
                            if (guard) guard->reevaluationFinished(
                                *refreshed, *error);
                        }, Qt::QueuedConnection);
                }
            });
        reevaluation_thread_ = thread;
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    void reevaluationFinished(const AutonomyQueue& refreshed,
                              const std::string& error) {
        reevaluating_ = false;
        reevaluation_thread_.clear();
        table_->setEnabled(true);
        apply_certified_button_->setEnabled(true);
        refresh_button_->setEnabled(true);
        refresh_button_->setText(QStringLiteral("Re-evaluate evidence"));
        if (!error.empty()) {
            refreshTable();
            QMessageBox::critical(this, QStringLiteral("Re-evaluation failed"),
                                  QString::fromStdString(error));
            return;
        }
        queue_ = refreshed;
        persist();
        refreshTable();
    }

    void applyCertified() {
        if (reevaluating_ || operation_running_) return;
        AutonomyPolicy policy = queue_.policy;
        policy.mode = AutonomyMode::automatic;
        reevaluate_pending_autonomy_queue(session_, queue_, policy);
        refresh_autonomy_queue_counts(queue_);
        persist();
        refreshTable();
        if (queue_.auto_apply_count == 0) {
            QMessageBox::information(this, QStringLiteral("No certified changes"),
                QStringLiteral("No pending operation satisfies the current automatic policy. ")
                + QStringLiteral("Items requiring judgment remain in human review."));
            return;
        }
        const QString confirmation = QStringLiteral(
            "Apply %1 certified %2?\n\n"
            "The engine will regenerate evidence before each mutation and stop "
            "when an item becomes stale, blocked, or exceeds policy.")
            .arg(queue_.auto_apply_count)
            .arg(queue_.auto_apply_count == 1
                ? QStringLiteral("change") : QStringLiteral("changes"));
        if (QMessageBox::question(this, QStringLiteral("Confirm certified changes"),
                confirmation, QMessageBox::Apply | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Apply) {
            return;
        }
        execute_autonomy_queue(session_, queue_, policy);
        persist();
        refreshTable();
    }

    void reviewSelected() {
        if (preparing_review_ || reevaluating_ || operation_running_) return;
        if (active_aux_review_) {
            active_aux_review_->raise();
            active_aux_review_->activateWindow();
            return;
        }
        if (active_decision_) {
            active_decision_->raise();
            active_decision_->activateWindow();
            return;
        }
        const int row = table_->currentRow();
        if (row < 0 || row >= static_cast<int>(queue_.items.size())) return;
        AutonomyQueueItem& item = queue_.items[row];
        if (isStale(item)) {
            QMessageBox::information(this, QStringLiteral("Proposal is stale"),
                QStringLiteral("The document revision changed after this proposal "
                               "was evaluated. Re-evaluate evidence before review."));
            return;
        }
        if (item.executed || item.rejected
            || item.disposition != AutonomyDisposition::human_review) {
            QMessageBox::information(this, QStringLiteral("No review needed"),
                QStringLiteral("Select an unresolved human-review item."));
            return;
        }
        if (transactionalReviewOperation(item.proposal.op)) {
            reviewTransactionalOperation(item);
            return;
        }
        if (item.proposal.op.rfind("coverage_", 0) == 0) {
            try {
                if (queue_.coverage_certificate_path.empty()) {
                    throw std::runtime_error(
                        "coverage queue does not reference its source certificate");
                }
                AleaCoverageCertificate certificate =
                    read_alea_coverage_certificate_json(
                        queue_.coverage_certificate_path);
                if (certificate.certificate_digest
                    != queue_.coverage_certificate_digest) {
                    throw std::runtime_error(
                        "coverage certificate changed since this queue was created");
                }
                auto* review = new CoverageReviewWindow(
                    std::move(certificate), queue_.coverage_certificate_path);
                review->setAttribute(Qt::WA_DeleteOnClose);
                active_aux_review_ = review;
                connect(review, &QObject::destroyed, this, [this]() {
                    active_aux_review_.clear();
                });
                review->show();
            } catch (const std::exception& error) {
                QMessageBox::critical(this,
                    QStringLiteral("Coverage review unavailable"),
                    QString::fromStdString(error.what()));
            }
            return;
        }
        if (item.proposal.op == "apply_contained_medium"
            && item.proposal.medium_request) {
            MediumGenerationRequest edited = *item.proposal.medium_request;
            MediumGenerationProposal certified;
            if (!edit_medium_request(
                    this, session_, queue_.document_id, queue_.policy,
                    &edited, &certified)) {
                return;
            }
            const CommandResult staged = stage_contained_medium_request(
                session_, queue_.document_id, edited);
            if (!staged.ok) {
                QMessageBox::critical(this,
                    QStringLiteral("Could not stage medium request"),
                    QStringLiteral("The edited request was not persisted; "
                                   "the previous queue item is unchanged."));
                return;
            }
            item.proposal.medium_request = edited;
            item.evidence.confidence = "certified";
            item.evidence.usable = true;
            item.evidence.exact = true;
            item.evidence.certificate_passed = true;
            item.evidence.volume_fraction =
                certified.certificate.generated_volume;
            item.evaluated_revision = document_revision(
                session_, queue_.document_id);
            persist();
        }
        preparing_review_ = true;
        review_button_->setEnabled(false);
        summary_->setText(QStringLiteral("Preparing fresh OCCT proposal and geometry review…"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const auto preparationFinished = [this]() {
            QApplication::restoreOverrideCursor();
            preparing_review_ = false;
            review_button_->setEnabled(true);
        };
        try {
        ViewerReviewContext review;
        review.review_id = item.proposal.id;
        review.action = item.proposal.op;
        review.target_id = item.proposal.target_id;
        review.candidate_id = item.proposal.candidate_id;
        review.audit.actor = "human";
        review.audit.proposal_actor = item.proposal.actor;
        review.audit.proposal_id = item.proposal.id;
        review.audit.rationale = item.proposal.rationale;
        review.audit.policy_rule = item.policy_rule;
        review.audit.approval_mode = "manual";
        std::string preselect = item.proposal.target_id;
        review_proposal_ = {};
        const DecompositionProposal* proposal = nullptr;
        if (item.proposal.op == "apply_decomposition_candidate") {
            review.kind = "decomposition";
            review_proposal_ = propose_decomposition(session_, item.proposal.target_id);
            proposal = &review_proposal_;
        } else if (item.proposal.op == "apply_surface_replacement_candidate") {
            review.kind = "surface_replacement";
            const SurfaceReplacementProposal surface =
                propose_surface_replacement(session_, item.proposal.target_id);
            for (const SurfaceReplacementCandidate& candidate : surface.candidates) {
                if (candidate.id == item.proposal.candidate_id) {
                    review.surface_replacement = candidate;
                    review.has_surface_replacement = true;
                    preselect = candidate.owner_target_id;
                    break;
                }
            }
        } else if (item.proposal.op == "remove_solid") {
            review.kind = "removal";
        } else if (item.proposal.op == "apply_contained_medium"
                   && item.proposal.medium_request) {
            review.kind = "contained_medium";
            review.medium_request = *item.proposal.medium_request;
            review.candidate_id = review.medium_request.id;
            review.medium_proposal = propose_contained_medium(
                session_, queue_.document_id, review.medium_request);
            if (!review.medium_proposal.ok) {
                throw std::runtime_error(
                    "fresh contained-medium geometry certificate failed");
            }
            review.has_medium = true;
            preselect = review.medium_proposal.host_shape_id;
        } else {
            throw std::runtime_error(
                "this operation has no Qt geometry-review adapter yet");
        }
        const std::string item_id = item.proposal.id;
        auto* decision = new DecisionWindow(
            session_, step_path_, queue_.document_id, preselect, proposal,
            item.proposal.candidate_id, review, std::string(),
            [this, item_id](bool accepted, const std::string& chosen_candidate,
                            bool preview_certified) {
                for (AutonomyQueueItem& current : queue_.items) {
                    if (current.proposal.id != item_id) continue;
                    if (accepted) {
                        if (!chosen_candidate.empty())
                            current.proposal.candidate_id = chosen_candidate;
                        if (current.proposal.op
                            == "apply_decomposition_candidate") {
                            const DecompositionCandidate* chosen = find_candidate(
                                &review_proposal_, chosen_candidate);
                            if (chosen) {
                                current.evidence.confidence = chosen->confidence;
                                current.evidence.usable = chosen->usable;
                                current.evidence.exact = chosen->exact_piece_count
                                    == chosen->piece_count;
                                current.evidence.certificate_passed =
                                    preview_certified;
                                current.evidence.relative_volume_delta =
                                    chosen->relative_delta;
                                current.evidence.piece_count = chosen->piece_count;
                                current.evidence.residual_count =
                                    chosen->pieces_requiring_decomposition
                                    + chosen->pieces_requiring_spline_replacement
                                    + chosen->pieces_requiring_user_decision;
                                current.evidence.sliver_count =
                                    chosen->sliver_piece_count;
                            }
                        }
                        current.executed = true;
                        current.execution_ok = true;
                        current.review_rationale = "accepted by human reviewer";
                    } else {
                        reject_autonomy_queue_item(queue_, item_id,
                            "rejected in Qt geometry reviewer");
                    }
                    break;
                }
                reevaluate_pending_autonomy_queue(session_, queue_, queue_.policy);
                persist();
                refreshTable();
            }, false, project_);
        decision->setAttribute(Qt::WA_DeleteOnClose);
        active_decision_ = decision;
        connect(decision, &QObject::destroyed, this, [this]() {
            active_decision_.clear();
        });
        decision->show();
        preparationFinished();
        } catch (const std::bad_alloc&) {
            preparationFinished();
            refreshTable();
            QMessageBox::critical(this, QStringLiteral("Review memory failure"),
                QStringLiteral("OCCT could not allocate the detailed review. "
                               "The queue and source geometry are unchanged."));
        } catch (const std::exception& error) {
            preparationFinished();
            refreshTable();
            QMessageBox::critical(this, QStringLiteral("Review preparation failed"),
                QString::fromStdString(error.what()));
        } catch (...) {
            preparationFinished();
            refreshTable();
            QMessageBox::critical(this, QStringLiteral("Review preparation failed"),
                QStringLiteral("OCCT rejected the review preparation. "
                               "The queue and source geometry are unchanged."));
        }
    }

    Session& session_;
    std::string step_path_;
    AutonomyQueue& queue_;
    std::string queue_path_;
    QLabel* summary_ = nullptr;
    QLabel* authority_ = nullptr;
    QTextBrowser* explanation_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* review_button_ = nullptr;
    QPushButton* apply_certified_button_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    CadJobController* job_controller_ = nullptr;
    DecompositionProposal review_proposal_;
    QPointer<DecisionWindow> active_decision_;
    QPointer<QWidget> active_aux_review_;
    QPointer<QThread> reevaluation_thread_;
    bool preparing_review_ = false;
    bool reevaluating_ = false;
    bool operation_running_ = false;
    bool pending_reevaluation_ = false;
    std::string active_operation_id_;
    ProjectStore* project_ = nullptr;
};

class ProjectPolicyDialog final : public QDialog {
public:
    explicit ProjectPolicyDialog(const AutonomyProjectPolicy& policy,
                                 QWidget* parent = nullptr)
        : QDialog(parent), result_(policy) {
        setWindowTitle(QStringLiteral("Edit project policy"));
        resize(620, 680);
        auto* layout = new QVBoxLayout(this);
        auto* heading = new QLabel(QStringLiteral(
            "These settings define the geometry imported for every later decision. "
            "Saving a change requires the active model to be loaded again."), this);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        auto* content = new QWidget(scroll);
        auto* content_layout = new QVBoxLayout(content);
        scroll->setWidget(content);
        layout->addWidget(scroll, 1);

        auto* identity = new QGroupBox(QStringLiteral("Purpose and preparation"), this);
        auto* identity_form = new QFormLayout(identity);
        auto* project_id = new QLabel(QString::fromStdString(policy.project_id), identity);
        project_id->setTextInteractionFlags(Qt::TextSelectableByMouse);
        intended_analysis_ = new QLineEdit(
            QString::fromStdString(policy.intended_analysis), identity);
        heal_shape_ = new QCheckBox(QStringLiteral("Heal imported shapes"), identity);
        heal_shape_->setChecked(policy.preparation.heal_shape);
        unit_override_ = editableUnits(policy.preparation.unit_override, identity);
        unit_scale_ = numberEditor(identity, 1.0e12);
        unit_scale_->setValue(policy.preparation.unit_scale_to_mm_override);
        normalize_unit_ = editableUnits(policy.preparation.normalize_to_unit, identity);
        identity_form->addRow(QStringLiteral("Project"), project_id);
        identity_form->addRow(QStringLiteral("Intended analysis"), intended_analysis_);
        identity_form->addRow(QString(), heal_shape_);
        identity_form->addRow(QStringLiteral("Assert source unit"), unit_override_);
        identity_form->addRow(QStringLiteral("Custom scale to mm (0 = automatic)"), unit_scale_);
        identity_form->addRow(QStringLiteral("Normalize geometry to"), normalize_unit_);
        content_layout->addWidget(identity);

        auto* authority = new QGroupBox(QStringLiteral("Authority"), this);
        auto* authority_form = new QFormLayout(authority);
        authority_mode_ = new QComboBox(authority);
        authority_mode_->addItem(QStringLiteral("Advisory"),
                                 static_cast<int>(AutonomyMode::explain));
        authority_mode_->addItem(QStringLiteral("Assisted"),
                                 static_cast<int>(AutonomyMode::propose));
        authority_mode_->addItem(QStringLiteral("Delegated"),
                                 static_cast<int>(AutonomyMode::automatic));
        authority_mode_->setCurrentIndex(authority_mode_->findData(
            static_cast<int>(policy.authority.mode)));
        high_confidence_ = check(QStringLiteral(
            "Allow high-confidence exact decomposition"),
            policy.authority.allow_high_confidence_decomposition, authority);
        certified_intermediate_ = check(QStringLiteral(
            "Allow certified intermediate decomposition"),
            policy.authority.allow_certified_intermediate_decomposition, authority);
        exact_surface_ = check(QStringLiteral("Allow exact surface replacement"),
            policy.authority.allow_exact_surface_replacement, authority);
        approximate_surface_ = check(QStringLiteral(
            "Allow budgeted approximate surface replacement"),
            policy.authority.allow_approximate_surface_replacement, authority);
        removal_ = check(QStringLiteral("Allow explicit removal"),
            policy.authority.allow_removal, authority);
        authority_form->addRow(QStringLiteral("Mode"), authority_mode_);
        authority_form->addRow(QString(), high_confidence_);
        authority_form->addRow(QString(), certified_intermediate_);
        authority_form->addRow(QString(), exact_surface_);
        authority_form->addRow(QString(), approximate_surface_);
        authority_form->addRow(QString(), removal_);
        content_layout->addWidget(authority);

        auto* approximation = new QGroupBox(
            QStringLiteral("Approximation budget"), this);
        auto* approximation_form = new QFormLayout(approximation);
        approximation_enabled_ = check(QStringLiteral(
            "Enable explicitly budgeted approximation"),
            policy.authority.approximation.enabled, approximation);
        global_limit_ = fractionEditor(approximation);
        body_limit_ = fractionEditor(approximation);
        action_limit_ = fractionEditor(approximation);
        surface_limit_ = numberEditor(approximation, 1.0e12);
        global_limit_->setValue(
            policy.authority.approximation.max_global_changed_volume_fraction);
        body_limit_->setValue(
            policy.authority.approximation.max_body_changed_volume_fraction);
        action_limit_->setValue(
            policy.authority.approximation.max_action_changed_volume_fraction);
        surface_limit_->setValue(policy.authority.approximation.max_surface_deviation);
        approximate_surface_changes_ = check(QStringLiteral(
            "Allow measured surface replacement"),
            policy.authority.approximation.allow_surface_replacement,
            approximation);
        approximate_surface_changes_->setObjectName(
            QStringLiteral("allowMeasuredSurfaceReplacementCheck"));
        shatter_loss_ = check(QStringLiteral(
            "Allow measured numerical shatter loss"),
            policy.authority.approximation.allow_numerical_shatter_loss,
            approximation);
        shatter_loss_->setObjectName(
            QStringLiteral("allowNumericalShatterLossCheck"));
        residual_removal_ = check(QStringLiteral(
            "Allow measured residual removal"),
            policy.authority.approximation.allow_sliver_removal,
            approximation);
        residual_removal_->setObjectName(
            QStringLiteral("allowResidualRemovalCheck"));
        overlap_resolution_ = check(QStringLiteral(
            "Allow measured overlap ownership resolution"),
            policy.authority.approximation.allow_overlap_resolution,
            approximation);
        overlap_resolution_->setObjectName(
            QStringLiteral("allowOverlapResolutionCheck"));
        preserve_topology_ = check(QStringLiteral(
            "Require topology preservation"),
            policy.authority.approximation.require_topology_preservation,
            approximation);
        preserve_containment_ = check(QStringLiteral(
            "Require containment preservation"),
            policy.authority.approximation.require_containment_preservation,
            approximation);
        approximation_form->addRow(QString(), approximation_enabled_);
        approximation_form->addRow(QStringLiteral("Global changed-volume fraction"),
                                   global_limit_);
        approximation_form->addRow(QStringLiteral("Per-body changed-volume fraction"),
                                   body_limit_);
        approximation_form->addRow(QStringLiteral("Per-action changed-volume fraction"),
                                   action_limit_);
        approximation_form->addRow(QStringLiteral("Surface deviation (model units)"),
                                   surface_limit_);
        approximation_form->addRow(QString(), approximate_surface_changes_);
        approximation_form->addRow(QString(), shatter_loss_);
        approximation_form->addRow(QString(), residual_removal_);
        approximation_form->addRow(QString(), overlap_resolution_);
        approximation_form->addRow(QString(), preserve_topology_);
        approximation_form->addRow(QString(), preserve_containment_);
        const std::size_t protected_count =
            policy.authority.approximation.protected_target_ids.size();
        const std::size_t protected_material_count =
            policy.authority.approximation.protected_material_ids.size();
        auto* retained = new QLabel(QStringLiteral(
            "%1 protected target(s), %2 protected material(s), and %3 contained-medium "
            "material definition(s) are retained unchanged.")
            .arg(protected_count).arg(protected_material_count)
            .arg(policy.authority.contained_medium_materials.size()), approximation);
        retained->setWordWrap(true);
        approximation_form->addRow(QStringLiteral("Preserved advanced policy"), retained);
        content_layout->addWidget(approximation);
        content_layout->addStretch();

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (intended_analysis_->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Analysis purpose required"),
                    QStringLiteral("Enter a short intended analysis before saving."));
                return;
            }
            updateResult();
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    const AutonomyProjectPolicy& result() const { return result_; }

private:
    static QCheckBox* check(const QString& label, bool checked, QWidget* parent) {
        auto* value = new QCheckBox(label, parent);
        value->setChecked(checked);
        return value;
    }

    static QDoubleSpinBox* numberEditor(QWidget* parent, double maximum) {
        auto* value = new QDoubleSpinBox(parent);
        value->setDecimals(12);
        value->setRange(0.0, maximum);
        value->setSingleStep(1.0e-6);
        return value;
    }

    static QDoubleSpinBox* fractionEditor(QWidget* parent) {
        return numberEditor(parent, 1.0);
    }

    static QComboBox* editableUnits(const std::string& current, QWidget* parent) {
        auto* value = new QComboBox(parent);
        value->setEditable(true);
        value->addItems({QString(), QStringLiteral("mm"), QStringLiteral("cm"),
                         QStringLiteral("m"), QStringLiteral("in"),
                         QStringLiteral("ft")});
        value->setCurrentText(QString::fromStdString(current));
        return value;
    }

    void updateResult() {
        result_.intended_analysis = intended_analysis_->text().trimmed().toStdString();
        result_.preparation.heal_shape = heal_shape_->isChecked();
        result_.preparation.unit_override =
            unit_override_->currentText().trimmed().toStdString();
        result_.preparation.unit_scale_to_mm_override = unit_scale_->value();
        result_.preparation.normalize_to_unit =
            normalize_unit_->currentText().trimmed().toStdString();
        result_.authority.mode = static_cast<AutonomyMode>(
            authority_mode_->currentData().toInt());
        result_.authority.allow_high_confidence_decomposition =
            high_confidence_->isChecked();
        result_.authority.allow_certified_intermediate_decomposition =
            certified_intermediate_->isChecked();
        result_.authority.allow_exact_surface_replacement = exact_surface_->isChecked();
        result_.authority.allow_approximate_surface_replacement =
            approximate_surface_->isChecked();
        result_.authority.allow_removal = removal_->isChecked();
        result_.authority.approximation.enabled = approximation_enabled_->isChecked();
        result_.authority.approximation.max_global_changed_volume_fraction =
            global_limit_->value();
        result_.authority.approximation.max_body_changed_volume_fraction =
            body_limit_->value();
        result_.authority.approximation.max_action_changed_volume_fraction =
            action_limit_->value();
        result_.authority.approximation.max_surface_deviation =
            surface_limit_->value();
        result_.authority.approximation.allow_surface_replacement =
            approximate_surface_changes_->isChecked();
        result_.authority.approximation.allow_numerical_shatter_loss =
            shatter_loss_->isChecked();
        result_.authority.approximation.allow_sliver_removal =
            residual_removal_->isChecked();
        result_.authority.approximation.allow_overlap_resolution =
            overlap_resolution_->isChecked();
        result_.authority.approximation.require_topology_preservation =
            preserve_topology_->isChecked();
        result_.authority.approximation.require_containment_preservation =
            preserve_containment_->isChecked();
    }

    AutonomyProjectPolicy result_;
    QLineEdit* intended_analysis_ = nullptr;
    QCheckBox *heal_shape_ = nullptr, *high_confidence_ = nullptr,
              *certified_intermediate_ = nullptr, *exact_surface_ = nullptr,
              *approximate_surface_ = nullptr, *removal_ = nullptr,
              *approximation_enabled_ = nullptr,
              *approximate_surface_changes_ = nullptr,
              *shatter_loss_ = nullptr, *residual_removal_ = nullptr,
              *overlap_resolution_ = nullptr, *preserve_topology_ = nullptr,
              *preserve_containment_ = nullptr;
    QComboBox *unit_override_ = nullptr, *normalize_unit_ = nullptr,
              *authority_mode_ = nullptr;
    QDoubleSpinBox *unit_scale_ = nullptr, *global_limit_ = nullptr,
                   *body_limit_ = nullptr, *action_limit_ = nullptr,
                   *surface_limit_ = nullptr;
};

class ProjectWorkspaceWindow final : public QMainWindow {
public:
    ProjectWorkspaceWindow(std::string project_path,
                           std::string source_path,
                           std::string coverage_path,
                           std::string queue_path)
        : project_(std::move(project_path)), source_path_(std::move(source_path)),
          coverage_path_(std::move(coverage_path)), queue_path_(std::move(queue_path)) {
        std::string error;
        if (!project_.open(&error)) throw std::runtime_error(error);
        policy_path_ = (project_.root() / "project-policy.json").string();
        std::error_code policy_error;
        if (!std::filesystem::exists(policy_path_, policy_error)) {
            std::string project_id = project_.root().filename().string();
            if (project_id.empty()) project_id = "csgify-project";
            if (policy_error || write_default_autonomy_project_policy_json(
                    policy_path_, project_id) != 0)
                throw std::runtime_error("could not create the project policy");
        }
        policy_ = read_autonomy_project_policy_json(policy_path_);
        load_options_ = conversionOptionsForProject(policy_);
        setWindowTitle(QStringLiteral("csgify project workspace — %1")
                           .arg(QString::fromStdString(project_.root().string())));
        resize(1220, 780);

        auto* tabs = new QTabWidget(this);
        setCentralWidget(tabs);
        tabs->addTab(makeOverviewTab(), QStringLiteral("Overview"));
        tabs->addTab(makeAssemblyTab(), QStringLiteral("Assembly"));
        tabs->addTab(makeSpatialTab(), QStringLiteral("Spatial focus"));
        tabs->addTab(makeJobsTab(), QStringLiteral("Jobs"));
        tabs->addTab(makeArtifactsTab(), QStringLiteral("Artifacts"));
        tabs->addTab(makeReviewTab(), QStringLiteral("Coverage & autonomy"));

        geometry_load_controller_ = new CadJobController(this);
        if (qEnvironmentVariable("CSGIFY_ENABLE_EMBEDDED_CHAT") == QStringLiteral("1"))
            initializeEmbeddedConversation();
        geometry_load_dock_ = new JobProgressDock(this);
        geometry_load_dock_->setWindowTitle(QStringLiteral("Model loading"));
        addDockWidget(Qt::BottomDockWidgetArea, geometry_load_dock_);
        geometry_load_dock_->hide();
        connect(geometry_load_controller_, &CadJobController::progress,
                geometry_load_dock_, &JobProgressDock::appendEvent);
        connect(geometry_load_dock_, &JobProgressDock::cancelRequested,
                geometry_load_controller_, &CadJobController::cancel);
        connect(geometry_load_controller_, &CadJobController::documentLoaded,
                this, [this](const LoadedDocumentPtr& loaded) {
                    if (!loaded || !loaded->session) return;
                    session_ = std::move(loaded->session);
                    document_id_ = loaded->document_id;
                    geometry_load_succeeded_ = true;
                    if (agent_dock_) {
                        agent_dock_->setAvailable(true);
                        agent_dock_->setContext(
                            QString::fromStdString(document_id_),
                            document_revision(*session_, document_id_));
                    }
                    const bool reconstructed = loaded->reconstructed;
                    reconstructing_load_ = false;
                    const ProjectSnapshot snapshot = project_snapshot_from_report(
                        loaded->analysis.report);
                    std::string persist_error;
                    if (!project_.persist_snapshot(snapshot, &persist_error)
                        || !project_.replace_spatial_entries(
                            snapshot.source_digest,
                            spatial_entries_from_report(loaded->analysis.report),
                            &persist_error)
                        || !project_.sync_definition_recognition_cache(
                            loaded->analysis.report, load_options_, nullptr,
                            &persist_error)) {
                        session_.reset();
                        document_id_.clear();
                        if (agent_dock_) agent_dock_->setAvailable(false);
                        geometry_load_succeeded_ = false;
                        evaluated_readiness_ = QStringLiteral(
                            "Import failed — the project index could not be updated.");
                        if (readiness_) readiness_->setText(evaluated_readiness_);
                        QMessageBox::critical(this,
                            QStringLiteral("Project index update failed"),
                            QString::fromStdString(persist_error));
                        return;
                    }
                    std::size_t blockers = 0;
                    for (const Diagnostic& diagnostic
                         : loaded->analysis.report.diagnostics) {
                        if (diagnostic.severity == DiagnosticSeverity::error)
                            ++blockers;
                    }
                    evaluated_readiness_ = loaded->analysis.report.ok()
                        ? reconstructed
                            ? QStringLiteral("Restored %1 saved operation(s) at revision %2 — evidence regenerated.")
                                .arg(loaded->reconstructed_operations)
                                .arg(document_revision(*session_, document_id_))
                            : QStringLiteral("Initial analysis passed — review or export the active model.")
                        : QStringLiteral("Needs review — %1 blocking diagnostic(s) found.")
                            .arg(blockers);
                    if (readiness_) readiness_->setText(evaluated_readiness_);
                    if (log_path_) {
                        log_path_->setEnabled(false);
                        log_path_->setToolTip(QStringLiteral(
                            "This decision log is attached to the active model session."));
                    }
                    resetProjectIndexes();
                    QTimer::singleShot(0, this,
                        [this]() { continuePendingGeometryAction(); });
                });
        connect(geometry_load_controller_, &CadJobController::failed,
                this, [this](const QString&, const QString& message) {
                    pending_geometry_action_ = PendingGeometryAction::none;
                    const bool exporting = export_kind_ != ExportKind::none;
                    const bool reconstructing = reconstructing_load_;
                    reconstructing_load_ = false;
                    evaluated_readiness_ = exporting
                        ? QStringLiteral("Export failed — the active project is unchanged.")
                        : reconstructing
                            ? QStringLiteral("Restore blocked — source, policy, or saved evidence did not match.")
                        : QStringLiteral(
                            "Model loading failed — readiness was not evaluated.");
                    export_kind_ = ExportKind::none;
                    export_path_.clear();
                    if (readiness_) readiness_->setText(evaluated_readiness_);
                    QMessageBox::critical(this, exporting
                        ? QStringLiteral("Export failed")
                        : reconstructing
                            ? QStringLiteral("Saved decisions not restored")
                        : QStringLiteral("Geometry load failed"), message);
                });
        connect(geometry_load_controller_, &CadJobController::commandCompleted,
                this, [this](const CommandResultPtr& result) {
                    if (export_kind_ == ExportKind::none || !result) return;
                    const ExportKind completed_kind = export_kind_;
                    const std::string completed_path = export_path_;
                    export_kind_ = ExportKind::none;
                    export_path_.clear();
                    if (!result->ok) {
                        evaluated_readiness_ = QStringLiteral(
                            "Export blocked — inspect the geometry evidence and unresolved work.");
                        if (readiness_) readiness_->setText(evaluated_readiness_);
                        QMessageBox::warning(this, QStringLiteral("Export blocked"),
                            QStringLiteral("The engine did not certify this output. "
                                           "The project remains open for review."));
                        return;
                    }
                    evaluated_readiness_ = QStringLiteral("Export completed at revision %1: %2")
                        .arg(document_revision(*session_, document_id_))
                        .arg(QString::fromStdString(completed_path));
                    if (readiness_) readiness_->setText(evaluated_readiness_);
                    ArtifactRecord artifact;
                    std::string artifact_error;
                    const bool archived = project_.artifacts().publish_file(
                        completed_kind == ExportKind::step
                            ? "reviewed-step" : "mcnp-geometry",
                        completed_kind == ExportKind::step ? "step" : "i",
                        completed_path, &artifact, &artifact_error)
                        && project_.record_artifact(artifact, &artifact_error);
                    if (archived) {
                        artifacts_->setRowCount(0);
                        artifact_offset_ = 0;
                        loadMoreArtifacts();
                        refreshOverview();
                    } else {
                        QMessageBox::warning(this,
                            QStringLiteral("Export completed; archive failed"),
                            QStringLiteral("The requested output was written, but its "
                                           "immutable project copy could not be recorded: %1")
                                .arg(QString::fromStdString(artifact_error)));
                    }
                    QMessageBox::information(this,
                        completed_kind == ExportKind::step
                            ? QStringLiteral("STEP export complete")
                            : QStringLiteral("MCNP export complete"),
                        QString::fromStdString(completed_path));
                });
        connect(geometry_load_controller_, &CadJobController::activeChanged,
                this, [this](bool active) {
                    if (geometry_button_) geometry_button_->setEnabled(
                        !active && (session_ || !resolvedSource().empty()));
                    if (choose_source_button_) choose_source_button_->setEnabled(!active);
                    if (edit_policy_button_) edit_policy_button_->setEnabled(!active);
                    if (restore_button_) restore_button_->setEnabled(
                        !active && !session_ && decisionHistoryAvailable());
                    if (resume_review_button_) resume_review_button_->setEnabled(
                        !active && session_
                        && !latest_decision_reviews(
                            *session_, "pending").empty());
                    if (export_step_button_) export_step_button_->setEnabled(!active && session_);
                    if (export_mcnp_button_) export_mcnp_button_->setEnabled(!active && session_);
                    if (autonomy_button_) autonomy_button_->setEnabled(
                        !active && !queue_path_.empty());
                    if (!active && !geometry_load_succeeded_
                        && pending_geometry_action_ != PendingGeometryAction::none) {
                        pending_geometry_action_ = PendingGeometryAction::none;
                        evaluated_readiness_ = QStringLiteral(
                            "Model loading cancelled — readiness was not evaluated.");
                        if (readiness_) readiness_->setText(evaluated_readiness_);
                    }
                    if (!active && close_after_geometry_load_) {
                        close_after_geometry_load_ = false;
                        pending_geometry_action_ = PendingGeometryAction::none;
                        QTimer::singleShot(0, this, &QWidget::close);
                    }
                });
        refreshOverview();
        loadMoreDefinitions();
        loadMoreInstances();
        loadSpatial(true);
        loadMoreJobs();
        loadMoreArtifacts();
        refreshReviewState();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (agentBusy()) {
            close_after_agent_ = true;
            if (agent_controller_ && agent_controller_->active())
                agent_controller_->cancel();
#if CSGIFY_HAVE_OPENAI_PROVIDER
            if (openai_agent_provider_ && openai_agent_provider_->active())
                openai_agent_provider_->cancel();
#endif
            if (agent_dock_) agent_dock_->setBusy(
                true, QStringLiteral("Stopping response before close…"));
            event->ignore();
            return;
        }
        if (geometry_load_controller_ && geometry_load_controller_->active()) {
            if (!close_after_geometry_load_) {
                const auto answer = QMessageBox::question(this,
                    QStringLiteral("Model task still running"),
                    QStringLiteral("Request safe cancellation and close when "
                                   "the current import or analysis stops?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (answer == QMessageBox::Yes) {
                    close_after_geometry_load_ = true;
                    geometry_load_controller_->cancel();
                    evaluated_readiness_ = QStringLiteral(
                        "Cancellation requested — closing after a safe boundary.");
                    if (readiness_) readiness_->setText(evaluated_readiness_);
                }
            }
            event->ignore();
            return;
        }
        event->accept();
    }

private:
    enum class ExportKind { none, step, mcnp };

    void initializeEmbeddedConversation() {
        agent_controller_ = new CadJobController(this);
        agent_dock_ = new AgentConversationDock(this);
        addDockWidget(Qt::RightDockWidgetArea, agent_dock_);
#if CSGIFY_HAVE_OPENAI_PROVIDER
        const QString openai_key = QString::fromUtf8(qgetenv("OPENAI_API_KEY"));
        const QString openai_model = QString::fromUtf8(
            qgetenv("CSGIFY_OPENAI_MODEL"));
        openai_agent_provider_ = new OpenAiAgentProvider(
            openai_key, openai_model, this);
        if (openai_agent_provider_->configured()) {
            agent_dock_->setProvider(
                QStringLiteral("Evidence assistant · OpenAI %1 · advisory")
                    .arg(openai_model), false);
        }
        connect(openai_agent_provider_, &OpenAiAgentProvider::completed,
                this, [this](const AgentResponsePtr& response) {
                    completeLiveAgentResponse(response);
                });
        connect(openai_agent_provider_, &OpenAiAgentProvider::failed,
                this, [this](const QString& code, const QString& message) {
                    failLiveAgentResponse(code, message);
                });
        connect(openai_agent_provider_, &OpenAiAgentProvider::activeChanged,
                this, [this](bool) { refreshAgentActivity(); });
#endif
        connect(agent_dock_, &AgentConversationDock::askRequested,
                this, [this](const QString& question) {
                    askAgent(question);
                });
        connect(agent_dock_, &AgentConversationDock::stopRequested,
                this, [this]() {
                    if (agent_controller_ && agent_controller_->active())
                        agent_controller_->cancel();
#if CSGIFY_HAVE_OPENAI_PROVIDER
                    if (openai_agent_provider_
                        && openai_agent_provider_->active())
                        openai_agent_provider_->cancel();
#endif
                });
        connect(agent_controller_, &CadJobController::agentContextCompleted,
                this, [this](const AgentContextSnapshotPtr& context) {
                    answerAgentQuestion(context);
                });
        connect(agent_controller_, &CadJobController::failed,
                this, [this](const QString&, const QString& message) {
                    persistAgentTerminalMessage(
                        "Assistant unavailable: " + message.toStdString(),
                        ProjectAgentMessageState::failed, nullptr, {});
                    pending_agent_question_.clear();
                    pending_agent_snapshot_.reset();
                    if (agent_dock_) agent_dock_->appendError(message);
                });
        connect(agent_controller_, &CadJobController::activeChanged,
                this, [this](bool) { refreshAgentActivity(); });
        restoreAgentConversation();
    }


    bool agentBusy() const {
        if (agent_controller_ && agent_controller_->active()) return true;
#if CSGIFY_HAVE_OPENAI_PROVIDER
        if (openai_agent_provider_ && openai_agent_provider_->active())
            return true;
#endif
        return false;
    }

    void refreshAgentActivity(const QString& status = {}) {
        const bool active = agentBusy();
        if (!active && !pending_agent_question_.isEmpty()) {
#if CSGIFY_HAVE_OPENAI_PROVIDER
            const bool live = pending_agent_snapshot_
                && openai_agent_provider_
                && openai_agent_provider_->configured();
            persistAgentTerminalMessage(
                "Response stopped before completion.",
                ProjectAgentMessageState::interrupted,
                pending_agent_snapshot_.get(), {},
                live ? "openai" : "scripted",
                live ? openai_agent_provider_->model().toStdString()
                     : "csgify-evidence-demo-v1",
                !live);
#else
            persistAgentTerminalMessage(
                "Response stopped before completion.",
                ProjectAgentMessageState::interrupted,
                pending_agent_snapshot_.get(), {});
#endif
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            if (agent_dock_) agent_dock_->appendStopped();
        } else if (agent_dock_ && (active || !status.isEmpty())) {
            QString effective_status = status;
            if (effective_status.isEmpty() && active) {
                effective_status = QStringLiteral(
                    "Gathering project evidence…");
#if CSGIFY_HAVE_OPENAI_PROVIDER
                if (openai_agent_provider_
                    && openai_agent_provider_->active())
                    effective_status = QStringLiteral(
                        "Waiting for OpenAI response…");
#endif
            }
            agent_dock_->setBusy(active, effective_status);
        }
        if (geometry_button_) geometry_button_->setEnabled(
            !active && (session_ || !resolvedSource().empty()));
        if (export_step_button_)
            export_step_button_->setEnabled(!active && session_);
        if (export_mcnp_button_)
            export_mcnp_button_->setEnabled(!active && session_);
        if (autonomy_button_)
            autonomy_button_->setEnabled(!active && !queue_path_.empty());
        if (choose_source_button_) choose_source_button_->setEnabled(!active);
        if (edit_policy_button_) edit_policy_button_->setEnabled(!active);
        if (resume_review_button_) resume_review_button_->setEnabled(
            !active && session_
            && !latest_decision_reviews(*session_, "pending").empty());
        if (!active && close_after_agent_) {
            close_after_agent_ = false;
            QTimer::singleShot(0, this, &QWidget::close);
        }
    }

    static QTableWidget* table(const QStringList& headers, QWidget* parent) {
        auto* result = new QTableWidget(parent);
        result->setColumnCount(headers.size());
        result->setHorizontalHeaderLabels(headers);
        result->setSelectionBehavior(QAbstractItemView::SelectRows);
        result->setSelectionMode(QAbstractItemView::SingleSelection);
        result->setEditTriggers(QAbstractItemView::NoEditTriggers);
        result->horizontalHeader()->setStretchLastSection(true);
        result->verticalHeader()->setVisible(false);
        return result;
    }

    static std::int64_t agentEpochSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string nextAgentMessageId(const char* role) {
        const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return std::string("agent-message:") + role + ":"
            + std::to_string(tick) + ":"
            + std::to_string(++agent_message_counter_);
    }

    std::vector<AgentMessage> recentAgentMessages() const {
        constexpr std::size_t kMaximumMessages = 24;
        constexpr std::size_t kMaximumBytes = 32 * 1024;
        std::vector<AgentMessage> result;
        std::size_t bytes = 0;
        for (auto it = agent_messages_.rbegin();
             it != agent_messages_.rend() && result.size() < kMaximumMessages;
             ++it) {
            if (!result.empty() && bytes + it->text.size() > kMaximumBytes)
                break;
            bytes += it->text.size();
            result.push_back(*it);
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    void restoreAgentConversation() {
        const std::int64_t now = agentEpochSeconds();
        ProjectAgentConversationRecord conversation;
        conversation.conversation_id = agent_conversation_id_;
        conversation.title = "Project conversation";
        conversation.created_at = now;
        conversation.updated_at = now;
        std::string error;
        if (!project_.upsert_agent_conversation(conversation, &error))
            throw std::runtime_error(
                "could not initialize project conversation: " + error);

        const std::vector<ProjectAgentMessageRecord> messages =
            project_.list_agent_messages(
                agent_conversation_id_, 1000, 0, &error);
        if (!error.empty())
            throw std::runtime_error(
                "could not restore project conversation: " + error);
        agent_dock_->resetTranscript();
        agent_messages_.clear();
        for (const ProjectAgentMessageRecord& message : messages) {
            last_agent_message_id_ = message.message_id;
            ++agent_message_counter_;
            if (message.role == ProjectAgentMessageRole::user) {
                agent_dock_->appendUserMessage(
                    QString::fromStdString(message.text));
                agent_messages_.push_back(
                    {AgentMessageRole::user, message.text});
                continue;
            }
            AgentResponse response;
            response.kind = AgentResponseKind::answer;
            response.message = message.state
                    == ProjectAgentMessageState::interrupted
                ? "[Interrupted] " + message.text
                : message.state == ProjectAgentMessageState::failed
                    ? "[Failed] " + message.text : message.text;
            for (const ProjectAgentEvidenceReference& reference
                 : message.references) {
                response.references.push_back(
                    {reference.kind, reference.id, reference.label});
            }
            agent_dock_->appendResponse(response, message.simulated);
            agent_messages_.push_back(
                {AgentMessageRole::assistant, message.text});
        }
        if (agent_dock_) agent_dock_->setAvailable(false);
    }

    bool persistAgentUserMessage(const QString& question) {
        ProjectAgentMessageRecord message;
        message.message_id = nextAgentMessageId("user");
        message.conversation_id = agent_conversation_id_;
        message.parent_message_id = last_agent_message_id_;
        message.role = ProjectAgentMessageRole::user;
        message.text = question.toStdString();
        message.document_id = document_id_;
        message.document_revision = document_revision(*session_, document_id_);
        message.target_id = document_id_;
        message.created_at = agentEpochSeconds();
        std::string error;
        if (!project_.append_agent_message(message, &error)) {
            agent_dock_->appendError(QStringLiteral(
                "The question could not be saved: %1")
                .arg(QString::fromStdString(error)));
            return false;
        }
        pending_agent_user_message_id_ = message.message_id;
        last_agent_message_id_ = message.message_id;
        return true;
    }

    bool persistAgentTerminalMessage(
        const std::string& text,
        ProjectAgentMessageState state,
        const AgentContextSnapshot* snapshot,
        const std::vector<AgentEvidenceReference>& references,
        const std::string& provider_id = "scripted",
        const std::string& model_id = "csgify-evidence-demo-v1",
        bool simulated = true) {
        if (pending_agent_user_message_id_.empty()) return true;
        ProjectAgentMessageRecord message;
        message.message_id = nextAgentMessageId("assistant");
        message.conversation_id = agent_conversation_id_;
        message.parent_message_id = pending_agent_user_message_id_;
        message.role = ProjectAgentMessageRole::assistant;
        message.state = state;
        message.text = text;
        message.provider_id = provider_id;
        message.model_id = model_id;
        message.simulated = simulated;
        message.created_at = agentEpochSeconds();
        if (snapshot) {
            message.context_id = snapshot->context_id;
            message.context_digest = snapshot->context_digest;
            message.document_id = snapshot->document_id;
            message.document_revision = snapshot->document_revision;
            message.target_id = snapshot->target_id;
        } else if (session_ && !document_id_.empty()) {
            message.document_id = document_id_;
            message.document_revision = document_revision(*session_, document_id_);
            message.target_id = document_id_;
        }
        for (const AgentEvidenceReference& reference : references) {
            message.references.push_back(
                {reference.kind, reference.id, reference.label});
        }
        std::string error;
        if (!project_.append_agent_message(message, &error)) {
            if (agent_dock_) agent_dock_->appendError(QStringLiteral(
                "The response could not be saved: %1")
                .arg(QString::fromStdString(error)));
            return false;
        }
        last_agent_message_id_ = message.message_id;
        pending_agent_user_message_id_.clear();
        return true;
    }

    void askAgent(const QString& question) {
        if (!session_ || document_id_.empty()) {
            agent_dock_->appendError(
                QStringLiteral("Load the project model before asking a geometry question."));
            return;
        }
        if (agentBusy()) return;
        if (!persistAgentUserMessage(question)) return;
        pending_agent_question_ = question;
        agent_messages_.push_back(
            {AgentMessageRole::user, question.toStdString()});
        AutonomyContextOptions options;
        options.project_id = policy_.project_id;
        options.intended_analysis = policy_.intended_analysis;
        options.preparation = policy_.preparation;
        options.max_candidates = 16;
        options.max_diagnostics = 64;
        options.max_serialized_bytes = 256 * 1024;
        if (!agent_controller_->startAgentContext(
                *session_, document_id_, policy_.authority, options,
                autonomy_conversion_policy_digest(load_options_))) {
            pending_agent_question_.clear();
            agent_messages_.pop_back();
            persistAgentTerminalMessage(
                "The geometry session was busy before evidence gathering started.",
                ProjectAgentMessageState::failed, nullptr, {});
            agent_dock_->appendError(QStringLiteral(
                "The geometry session is busy. Try again after the active operation finishes."));
        }
    }

    void answerAgentQuestion(const AgentContextSnapshotPtr& snapshot) {
        if (!snapshot || !session_) {
            persistAgentTerminalMessage(
                "The evidence snapshot was unavailable.",
                ProjectAgentMessageState::failed, nullptr, {});
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            if (agent_dock_) agent_dock_->appendError(
                QStringLiteral("The evidence snapshot was unavailable."));
            return;
        }
        const AgentValidationResult current = validate_agent_context_snapshot(
            *session_, document_id_, policy_.authority, *snapshot,
            snapshot->target_id);
        if (!current.ok) {
            persistAgentTerminalMessage(
                current.message, ProjectAgentMessageState::failed,
                snapshot.get(), {});
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            agent_dock_->appendError(QString::fromStdString(current.message));
            return;
        }
        pending_agent_snapshot_ = snapshot;
#if CSGIFY_HAVE_OPENAI_PROVIDER
        if (openai_agent_provider_ && openai_agent_provider_->configured()) {
            AgentProviderRequest request;
            request.context = *snapshot;
            request.messages = recentAgentMessages();
            if (openai_agent_provider_->start(request)) {
                agent_dock_->setBusy(
                    true, QStringLiteral("Waiting for OpenAI response…"));
                return;
            }
            if (pending_agent_question_.isEmpty()) return;
            failLiveAgentResponse(
                QStringLiteral("provider_start"),
                QStringLiteral("The OpenAI provider could not start."));
            return;
        }
#endif
        const bool ready = snapshot->context_json.find(
            "\"summary\":{\"ok\":true") != std::string::npos;
        const std::string message = ready
            ? "The engine snapshot reports no blocking analysis diagnostics at "
              "this revision. This simulated provider verifies the embedded "
              "question-and-evidence path; a live model provider is not configured yet."
            : "The engine snapshot reports unresolved geometry evidence at this "
              "revision. Review the diagnostics before export. This answer comes "
              "from the simulated provider; a live model provider is not configured yet.";
        const std::string response_json =
            "{\"schema\":\"csgify.agent-response\",\"schema_version\":1,"
            "\"context_id\":\"" + snapshot->context_id
            + "\",\"kind\":\"answer\",\"message\":\"" + message
            + "\",\"references\":[{\"kind\":\"document\",\"id\":\""
            + snapshot->document_id + "\",\"label\":\"Engine context\"}]}";
        try {
            ScriptedAgentProvider provider({response_json});
            AgentProviderRequest request;
            request.context = *snapshot;
            request.messages = recentAgentMessages();
            const AgentResponse response = provider.respond(request);
            if (!persistAgentTerminalMessage(
                    response.message, ProjectAgentMessageState::complete,
                    snapshot.get(), response.references)) {
                pending_agent_question_.clear();
                return;
            }
            agent_messages_.push_back(
                {AgentMessageRole::assistant, response.message});
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            agent_dock_->setContext(
                QString::fromStdString(snapshot->target_id.empty()
                    ? snapshot->document_id : snapshot->target_id),
                snapshot->document_revision);
            agent_dock_->appendResponse(response);
        } catch (const std::exception& error) {
            persistAgentTerminalMessage(
                std::string("Provider response failed validation: ")
                    + error.what(),
                ProjectAgentMessageState::failed, snapshot.get(), {});
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            agent_dock_->appendError(QString::fromLocal8Bit(error.what()));
        }
    }

#if CSGIFY_HAVE_OPENAI_PROVIDER
    void completeLiveAgentResponse(const AgentResponsePtr& response) {
        if (!response || !pending_agent_snapshot_ || !session_
            || pending_agent_question_.isEmpty())
            return;
        const AgentValidationResult current = validate_agent_context_snapshot(
            *session_, document_id_, policy_.authority,
            *pending_agent_snapshot_, pending_agent_snapshot_->target_id);
        if (!current.ok) {
            persistAgentTerminalMessage(
                "The model changed before this answer completed: "
                    + current.message,
                ProjectAgentMessageState::failed,
                pending_agent_snapshot_.get(), {}, "openai",
                openai_agent_provider_->model().toStdString(), false);
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            agent_dock_->appendError(QStringLiteral(
                "The answer was discarded because its geometry evidence is stale. %1")
                .arg(QString::fromStdString(current.message)));
            return;
        }
        if (!persistAgentTerminalMessage(
                response->message, ProjectAgentMessageState::complete,
                pending_agent_snapshot_.get(), response->references, "openai",
                openai_agent_provider_->model().toStdString(), false)) {
            pending_agent_question_.clear();
            pending_agent_snapshot_.reset();
            return;
        }
        agent_messages_.push_back(
            {AgentMessageRole::assistant, response->message});
        agent_dock_->setContext(
            QString::fromStdString(pending_agent_snapshot_->target_id.empty()
                ? pending_agent_snapshot_->document_id
                : pending_agent_snapshot_->target_id),
            pending_agent_snapshot_->document_revision);
        pending_agent_question_.clear();
        pending_agent_snapshot_.reset();
        agent_dock_->appendResponse(*response, false);
    }

    void failLiveAgentResponse(const QString& code, const QString& message) {
        if (pending_agent_question_.isEmpty()) return;
        const std::string stored = "OpenAI provider failure ("
            + code.toStdString() + "): " + message.toStdString();
        persistAgentTerminalMessage(
            stored, ProjectAgentMessageState::failed,
            pending_agent_snapshot_.get(), {}, "openai",
            openai_agent_provider_
                ? openai_agent_provider_->model().toStdString() : std::string{},
            false);
        pending_agent_question_.clear();
        pending_agent_snapshot_.reset();
        if (agent_dock_) agent_dock_->appendError(message);
    }
#endif

    QWidget* makeOverviewTab() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        readiness_ = new QLabel(page);
        QFont readiness_font = readiness_->font();
        readiness_font.setBold(true);
        readiness_font.setPointSize(readiness_font.pointSize() + 2);
        readiness_->setFont(readiness_font);
        readiness_->setWordWrap(true);
        readiness_->setObjectName(QStringLiteral("projectReadinessBanner"));
        layout->addWidget(readiness_);
        overview_ = new QLabel(page);
        overview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(overview_);
        auto* agent_workflow = new QLabel(QStringLiteral(
            "Use Codex or Claude Code to discuss the model and propose changes. "
            "Inspect geometry and approve proposals in this workspace."), page);
        agent_workflow->setWordWrap(true);
        layout->addWidget(agent_workflow);

        source_summary_ = new QLabel(page);
        source_summary_->setObjectName(QStringLiteral("projectSourceSummary"));
        source_summary_->setWordWrap(true);
        source_summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(source_summary_);
        auto* source_actions = new QHBoxLayout;
        choose_source_button_ = new QPushButton(
            QStringLiteral("Choose STEP model…"), page);
        choose_source_button_->setObjectName(
            QStringLiteral("chooseProjectSourceButton"));
        edit_policy_button_ = new QPushButton(
            QStringLiteral("Edit project policy…"), page);
        edit_policy_button_->setObjectName(QStringLiteral("editProjectPolicyButton"));
        auto* reload_policy = new QPushButton(
            QStringLiteral("Reload project policy"), page);
        reload_policy->setObjectName(QStringLiteral("reloadProjectPolicyButton"));
        source_actions->addWidget(choose_source_button_);
        source_actions->addWidget(edit_policy_button_);
        source_actions->addWidget(reload_policy);
        source_actions->addStretch();
        layout->addLayout(source_actions);
        connect(choose_source_button_, &QPushButton::clicked,
                this, [this]() { chooseSource(); });
        connect(edit_policy_button_, &QPushButton::clicked,
                this, [this]() { editPolicy(); });
        connect(reload_policy, &QPushButton::clicked,
                this, [this]() { reloadPolicy(); });

        policy_summary_ = new QLabel(page);
        policy_summary_->setObjectName(QStringLiteral("projectPolicySummary"));
        policy_summary_->setWordWrap(true);
        policy_summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(policy_summary_);

        geometry_button_ = new QPushButton(
            QStringLiteral("Load model and evaluate readiness…"), page);
        geometry_button_->setObjectName(QStringLiteral("evaluateReadinessButton"));
        connect(geometry_button_, &QPushButton::clicked,
                this, [this]() { openGeometry(); });
        restore_button_ = new QPushButton(
            QStringLiteral("Restore saved decisions…"), page);
        restore_button_->setObjectName(QStringLiteral("restoreProjectDecisionsButton"));
        connect(restore_button_, &QPushButton::clicked,
                this, [this]() { restoreDecisions(); });
        resume_review_button_ = new QPushButton(
            QStringLiteral("Resume pending review…"), page);
        resume_review_button_->setObjectName(
            QStringLiteral("resumePendingProjectReviewButton"));
        resume_review_button_->setEnabled(false);
        connect(resume_review_button_, &QPushButton::clicked,
                this, [this]() { resumePendingReview(); });
        auto* load_actions = new QHBoxLayout;
        load_actions->addWidget(geometry_button_);
        load_actions->addWidget(restore_button_);
        load_actions->addWidget(resume_review_button_);
        load_actions->addStretch();
        layout->addLayout(load_actions);

        auto* export_actions = new QHBoxLayout;
        export_step_button_ = new QPushButton(QStringLiteral("Export STEP…"), page);
        export_mcnp_button_ = new QPushButton(QStringLiteral("Export MCNP…"), page);
        export_step_button_->setObjectName(QStringLiteral("exportProjectStepButton"));
        export_mcnp_button_->setObjectName(QStringLiteral("exportProjectMcnpButton"));
        export_step_button_->setEnabled(false);
        export_mcnp_button_->setEnabled(false);
        export_actions->addWidget(export_step_button_);
        export_actions->addWidget(export_mcnp_button_);
        export_actions->addStretch();
        layout->addLayout(export_actions);
        connect(export_step_button_, &QPushButton::clicked,
                this, [this]() { startExport(ExportKind::step); });
        connect(export_mcnp_button_, &QPushButton::clicked,
                this, [this]() { startExport(ExportKind::mcnp); });

        auto* form = new QFormLayout;
        log_path_ = new QLineEdit(QString::fromStdString(
            (project_.root() / "decisions.json").string()), page);
        recipe_path_ = new QLineEdit(QString::fromStdString(
            (project_.root() / "recipe.lua").string()), page);
        form->addRow(QStringLiteral("Decision log"), log_path_);
        form->addRow(QStringLiteral("Recipe output"), recipe_path_);
        layout->addLayout(form);
        connect(log_path_, &QLineEdit::textChanged,
                this, [this]() { refreshOverview(); });
        preserve_steps_ = new QCheckBox(
            QStringLiteral("Preserve intermediate STEP exports in recipe"), page);
        layout->addWidget(preserve_steps_);
        auto* compact = new QPushButton(QStringLiteral("Compact decision log"), page);
        connect(compact, &QPushButton::clicked, this, [this]() { compactRecipe(); });
        layout->addWidget(compact);
        layout->addStretch();
        return page;
    }

    QWidget* makeAssemblyTab() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->addWidget(new QLabel(
            QStringLiteral("Database-backed assembly index (100 rows per request; no CAD load)"), page));
        auto* search_row = new QHBoxLayout;
        assembly_search_ = new QLineEdit(page);
        assembly_search_->setPlaceholderText(
            QStringLiteral("Search names, IDs, shape IDs, materials, or geometry digests"));
        auto* search = new QPushButton(QStringLiteral("Search entire index"), page);
        auto* clear = new QPushButton(QStringLiteral("Clear"), page);
        search_row->addWidget(assembly_search_, 1);
        search_row->addWidget(search);
        search_row->addWidget(clear);
        layout->addLayout(search_row);
        definitions_ = table({QStringLiteral("Definition"), QStringLiteral("Name"),
            QStringLiteral("Instances"), QStringLiteral("Geometry digest"),
            QStringLiteral("Verified")}, page);
        instances_ = table({QStringLiteral("Instance"), QStringLiteral("Name"),
            QStringLiteral("Definition"), QStringLiteral("Parent"),
            QStringLiteral("Shape / material")}, page);
        layout->addWidget(definitions_);
        auto* more_definitions = new QPushButton(QStringLiteral("Load 100 more definitions"), page);
        connect(more_definitions, &QPushButton::clicked,
                this, [this]() { loadMoreDefinitions(); });
        layout->addWidget(more_definitions);
        layout->addWidget(instances_);
        auto* more_instances = new QPushButton(QStringLiteral("Load 100 more instances"), page);
        auto* focus_instance = new QPushButton(
            QStringLiteral("Open selected instance — focused LOD"), page);
        connect(more_instances, &QPushButton::clicked,
                this, [this]() { loadMoreInstances(); });
        auto* instance_buttons = new QHBoxLayout;
        instance_buttons->addWidget(more_instances);
        instance_buttons->addWidget(focus_instance);
        layout->addLayout(instance_buttons);
        connect(search, &QPushButton::clicked, this, [this]() { searchAssembly(); });
        connect(assembly_search_, &QLineEdit::returnPressed,
                this, [this]() { searchAssembly(); });
        connect(clear, &QPushButton::clicked, this, [this]() {
            assembly_search_->clear();
            definitions_->setRowCount(0);
            instances_->setRowCount(0);
            definition_offset_ = instance_offset_ = 0;
            loadMoreDefinitions();
            loadMoreInstances();
        });
        connect(focus_instance, &QPushButton::clicked,
                this, [this]() { openSelectedInstance(); });
        connect(instances_, &QTableWidget::cellDoubleClicked,
                this, [this](int, int) { openSelectedInstance(); });
        return page;
    }

    QWidget* makeSpatialTab() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        auto* filters = new QHBoxLayout;
        spatial_namespace_ = new QComboBox(page);
        spatial_kind_ = new QComboBox(page);
        spatial_kind_->addItems({QStringLiteral("all kinds"),
            QStringLiteral("instance_solid"), QStringLiteral("material_region_piece"),
            QStringLiteral("pipe_port")});
        spatial_search_ = new QLineEdit(page);
        spatial_search_->setPlaceholderText(QStringLiteral("Entity or owner ID"));
        auto* apply = new QPushButton(QStringLiteral("Apply"), page);
        filters->addWidget(spatial_namespace_);
        filters->addWidget(spatial_kind_);
        filters->addWidget(spatial_search_, 1);
        filters->addWidget(apply);
        layout->addLayout(filters);
        spatial_ = table({QStringLiteral("Entity"), QStringLiteral("Owner"),
            QStringLiteral("Kind"), QStringLiteral("Bounding box")}, page);
        layout->addWidget(spatial_);
        auto* buttons = new QHBoxLayout;
        auto* more = new QPushButton(QStringLiteral("Load 100 more"), page);
        auto* focus = new QPushButton(QStringLiteral("Open selected entity — focused LOD"), page);
        buttons->addWidget(more);
        buttons->addWidget(focus);
        layout->addLayout(buttons);
        std::string error;
        for (const std::string& value : project_.list_spatial_namespaces(&error))
            spatial_namespace_->addItem(QString::fromStdString(value));
        connect(apply, &QPushButton::clicked, this, [this]() { loadSpatial(true); });
        connect(spatial_search_, &QLineEdit::returnPressed,
                this, [this]() { loadSpatial(true); });
        connect(more, &QPushButton::clicked, this, [this]() { loadSpatial(false); });
        connect(focus, &QPushButton::clicked, this, [this]() { openSelectedSpatial(); });
        connect(spatial_, &QTableWidget::cellDoubleClicked,
                this, [this](int, int) { openSelectedSpatial(); });
        return page;
    }

    QWidget* makeJobsTab() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        jobs_ = table({QStringLiteral("Job"), QStringLiteral("State"),
            QStringLiteral("Operation"), QStringLiteral("Phase"),
            QStringLiteral("Progress"), QStringLiteral("Target"),
            QStringLiteral("Attempt"), QStringLiteral("Heartbeat / elapsed"),
            QStringLiteral("Error")}, page);
        layout->addWidget(jobs_);
        auto* buttons = new QHBoxLayout;
        auto* more = new QPushButton(QStringLiteral("Load 100 more"), page);
        auto* retry = new QPushButton(QStringLiteral("Retry selected failed job"), page);
        auto* cancel = new QPushButton(QStringLiteral("Cancel selected job"), page);
        auto* details = new QPushButton(QStringLiteral("Open job details"), page);
        auto* recover = new QPushButton(QStringLiteral("Recover stale running jobs"), page);
        buttons->addWidget(more);
        buttons->addWidget(retry);
        buttons->addWidget(cancel);
        buttons->addWidget(details);
        buttons->addWidget(recover);
        layout->addLayout(buttons);
        connect(more, &QPushButton::clicked, this, [this]() { loadMoreJobs(); });
        connect(retry, &QPushButton::clicked, this, [this]() { retrySelectedJob(); });
        connect(cancel, &QPushButton::clicked, this, [this]() { cancelSelectedJob(); });
        connect(details, &QPushButton::clicked, this, [this]() { showSelectedJob(); });
        connect(recover, &QPushButton::clicked, this, [this]() { recoverStaleJobs(); });
        connect(jobs_, &QTableWidget::cellDoubleClicked,
                this, [this](int, int) { showSelectedJob(); });
        return page;
    }

    QWidget* makeArtifactsTab() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        artifacts_ = table({QStringLiteral("Kind"), QStringLiteral("Digest"),
            QStringLiteral("Bytes"), QStringLiteral("Project-relative path")}, page);
        layout->addWidget(artifacts_);
        auto* more = new QPushButton(QStringLiteral("Load 100 more artifacts"), page);
        connect(more, &QPushButton::clicked, this, [this]() { loadMoreArtifacts(); });
        layout->addWidget(more);
        return page;
    }

    QWidget* makeReviewTab() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        review_state_ = new QPlainTextEdit(page);
        review_state_->setReadOnly(true);
        layout->addWidget(review_state_);
        auto* buttons = new QHBoxLayout;
        coverage_button_ = new QPushButton(QStringLiteral("Open coverage review"), page);
        autonomy_button_ = new QPushButton(QStringLiteral("Open decision queue"), page);
        auto* refresh = new QPushButton(QStringLiteral("Refresh files"), page);
        buttons->addWidget(coverage_button_);
        buttons->addWidget(autonomy_button_);
        buttons->addWidget(refresh);
        layout->addLayout(buttons);
        connect(coverage_button_, &QPushButton::clicked,
                this, [this]() { openCoverage(); });
        connect(autonomy_button_, &QPushButton::clicked,
                this, [this]() { openAutonomy(); });
        connect(refresh, &QPushButton::clicked, this, [this]() { refreshReviewState(); });
        return page;
    }

    void refreshOverview() {
        std::string error;
        const ProjectCounts c = project_.counts(&error);
        const ProjectJobCounts j = project_.job_counts(&error);
        if (readiness_) {
            QString readiness;
            if (c.sources == 0 && source_path_.empty()) {
                readiness = QStringLiteral(
                    "No model indexed — import a STEP model before review.");
            } else if (!evaluated_readiness_.isEmpty()) {
                readiness = evaluated_readiness_;
            } else if (j.running > 0 || j.queued > 0) {
                readiness = QStringLiteral(
                    "Work in progress — %1 running, %2 queued.")
                    .arg(j.running).arg(j.queued);
            } else if (j.failed > 0) {
                readiness = QStringLiteral(
                    "Needs attention — %1 project job(s) failed. Readiness is not verified.")
                    .arg(j.failed);
            } else {
                readiness = QStringLiteral(
                    "Readiness not evaluated — load the model to inspect blockers and recommendations.");
            }
            readiness_->setText(readiness);
        }
        if (geometry_button_) {
            const bool source_available = c.sources > 0 || !source_path_.empty();
            geometry_button_->setEnabled(source_available);
            geometry_button_->setToolTip(source_available ? QString()
                : QStringLiteral("No source STEP is attached or indexed."));
        }
        if (restore_button_) {
            const bool history = decisionHistoryAvailable();
            restore_button_->setEnabled(history && !session_
                && (!geometry_load_controller_ || !geometry_load_controller_->active()));
            restore_button_->setToolTip(history
                ? QStringLiteral("Reconstruct the source through the typed decision recipe. "
                                 "Source contents and preparation policy must match exactly.")
                : QStringLiteral("No project decision history is available yet."));
        }
        if (resume_review_button_) {
            const std::size_t pending = session_
                ? latest_decision_reviews(*session_, "pending").size() : 0;
            resume_review_button_->setEnabled(pending > 0
                && (!geometry_load_controller_
                    || !geometry_load_controller_->active()));
            resume_review_button_->setText(pending > 0
                ? QStringLiteral("Resume pending review (%1)…").arg(pending)
                : QStringLiteral("Resume pending review…"));
            resume_review_button_->setToolTip(pending > 0
                ? QStringLiteral("Regenerate current engine evidence before reopening the saved proposal.")
                : QStringLiteral("No restorable pending review is loaded."));
        }
        if (source_summary_) {
            const std::string source = resolvedSource();
            source_summary_->setText(source.empty()
                ? QStringLiteral("Source model: none selected")
                : QStringLiteral("Source model: %1")
                    .arg(QString::fromStdString(source)));
        }
        if (policy_summary_) {
            const QString units = !policy_.preparation.normalize_to_unit.empty()
                ? QString::fromStdString(policy_.preparation.normalize_to_unit)
                : !policy_.preparation.unit_override.empty()
                    ? QString::fromStdString(policy_.preparation.unit_override)
                    : QStringLiteral("source units");
            policy_summary_->setText(QStringLiteral(
                "Effective project policy — %1\n"
                "Analysis: %2 · Authority: %3 · Preparation: %4, %5 · Approximation: %6")
                .arg(QString::fromStdString(policy_.project_id))
                .arg(QString::fromStdString(policy_.intended_analysis))
                .arg(authorityModeText(policy_.authority.mode))
                .arg(policy_.preparation.heal_shape
                    ? QStringLiteral("heal geometry")
                    : QStringLiteral("keep source topology"))
                .arg(units)
                .arg(policy_.authority.approximation.enabled
                    ? QStringLiteral("budgeted") : QStringLiteral("disabled")));
        }
        overview_->setText(QStringLiteral(
            "Project database: %1\nSchema: %2\n\n"
            "Sources %3 · Definitions %4 · Instances %5 · Materials %6\n"
            "Artifacts %7 · Cached conversions %8 · Spatial entries %9\n"
            "Jobs: queued %10 · running %11 · succeeded %12 · failed %13 · cancelled %14\n\n"
            "Opening this workspace does not load OCCT geometry.")
            .arg(QString::fromStdString(project_.database_path().string()))
            .arg(project_.schema_version(&error)).arg(c.sources)
            .arg(c.component_definitions).arg(c.component_instances).arg(c.materials)
            .arg(c.artifacts).arg(c.conversion_cache_entries).arg(c.spatial_entities)
            .arg(j.queued).arg(j.running).arg(j.succeeded).arg(j.failed).arg(j.cancelled));
    }

    void loadMoreDefinitions() {
        std::string error;
        const auto rows = project_.list_component_definitions(100, definition_offset_, &error);
        for (const auto& value : rows) {
            const int row = definitions_->rowCount();
            definitions_->insertRow(row);
            const QString values[] = {QString::fromStdString(value.definition_id),
                QString::fromStdString(value.display_name), QString::number(value.instance_count),
                QString::fromStdString(value.geometry_digest),
                value.geometry_digest_verified ? QStringLiteral("yes") : QStringLiteral("no")};
            for (int col = 0; col < 5; ++col) definitions_->setItem(row, col, new QTableWidgetItem(values[col]));
        }
        definition_offset_ += rows.size();
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Assembly query"), QString::fromStdString(error));
    }

    void loadMoreInstances() {
        std::string error;
        const auto rows = project_.list_component_instances(100, instance_offset_, &error);
        for (const auto& value : rows) {
            const int row = instances_->rowCount();
            instances_->insertRow(row);
            const QString shape_material = QString::fromStdString(value.source_shape_id)
                + (value.material_override.empty() ? QString() : QStringLiteral(" / ") + QString::fromStdString(value.material_override));
            const QString values[] = {QString::fromStdString(value.instance_id),
                QString::fromStdString(value.display_name), QString::fromStdString(value.definition_id),
                QString::fromStdString(value.parent_assembly_id), shape_material};
            for (int col = 0; col < 5; ++col) {
                auto* item = new QTableWidgetItem(values[col]);
                item->setData(Qt::UserRole, QString::fromStdString(value.source_shape_id));
                instances_->setItem(row, col, item);
            }
        }
        instance_offset_ += rows.size();
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Assembly query"), QString::fromStdString(error));
    }

    void searchAssembly() {
        const std::string query = assembly_search_->text().trimmed().toStdString();
        if (query.empty()) {
            definitions_->setRowCount(0);
            instances_->setRowCount(0);
            definition_offset_ = instance_offset_ = 0;
            loadMoreDefinitions();
            loadMoreInstances();
            return;
        }
        std::string error;
        const auto definitions = project_.search_component_definitions(query, 500, 0, &error);
        const auto instances = project_.search_component_instances(query, 500, 0, &error);
        definitions_->setRowCount(0);
        instances_->setRowCount(0);
        for (const auto& value : definitions) {
            const int row = definitions_->rowCount();
            definitions_->insertRow(row);
            const QString values[] = {QString::fromStdString(value.definition_id),
                QString::fromStdString(value.display_name), QString::number(value.instance_count),
                QString::fromStdString(value.geometry_digest),
                value.geometry_digest_verified ? QStringLiteral("yes") : QStringLiteral("no")};
            for (int col = 0; col < 5; ++col)
                definitions_->setItem(row, col, new QTableWidgetItem(values[col]));
        }
        for (const auto& value : instances) {
            const int row = instances_->rowCount();
            instances_->insertRow(row);
            const QString shape_material = QString::fromStdString(value.source_shape_id)
                + (value.material_override.empty() ? QString() : QStringLiteral(" / ")
                   + QString::fromStdString(value.material_override));
            const QString values[] = {QString::fromStdString(value.instance_id),
                QString::fromStdString(value.display_name), QString::fromStdString(value.definition_id),
                QString::fromStdString(value.parent_assembly_id), shape_material};
            for (int col = 0; col < 5; ++col) {
                auto* item = new QTableWidgetItem(values[col]);
                item->setData(Qt::UserRole, QString::fromStdString(value.source_shape_id));
                instances_->setItem(row, col, item);
            }
        }
        definition_offset_ = definitions.size();
        instance_offset_ = instances.size();
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Assembly search"), QString::fromStdString(error));
    }

    void loadSpatial(bool reset) {
        if (!spatial_) return;
        if (reset) {
            spatial_->setRowCount(0);
            spatial_offset_ = 0;
        }
        if (!spatial_namespace_ || spatial_namespace_->currentText().isEmpty()) return;
        const std::string model_namespace = spatial_namespace_->currentText().toStdString();
        const std::string kind = spatial_kind_->currentIndex() <= 0
            ? std::string() : spatial_kind_->currentText().toStdString();
        const std::string search = spatial_search_->text().trimmed().toStdString();
        std::string error;
        const auto rows = project_.list_spatial_entries(
            model_namespace, 100, spatial_offset_, kind, search, &error);
        for (const auto& value : rows) {
            const int row = spatial_->rowCount();
            spatial_->insertRow(row);
            std::ostringstream bounds;
            bounds << '[' << value.bounds.xmin << ", " << value.bounds.ymin << ", "
                   << value.bounds.zmin << "] — [" << value.bounds.xmax << ", "
                   << value.bounds.ymax << ", " << value.bounds.zmax << ']';
            const QString values[] = {QString::fromStdString(value.entity_id),
                QString::fromStdString(value.owner_id), QString::fromStdString(value.kind),
                QString::fromStdString(bounds.str())};
            for (int col = 0; col < 4; ++col) {
                auto* item = new QTableWidgetItem(values[col]);
                item->setData(Qt::UserRole, QString::fromStdString(value.entity_id));
                spatial_->setItem(row, col, item);
            }
        }
        spatial_offset_ += rows.size();
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Spatial query"), QString::fromStdString(error));
    }

    void openSelectedInstance() {
        const int row = instances_->currentRow();
        if (row < 0 || !instances_->item(row, 0)) return;
        openGeometry(instances_->item(row, 0)->data(Qt::UserRole).toString().toStdString(), true);
    }

    void openSelectedSpatial() {
        const int row = spatial_->currentRow();
        if (row < 0 || !spatial_->item(row, 0)) return;
        openGeometry(spatial_->item(row, 0)->data(Qt::UserRole).toString().toStdString(), true);
    }

    void loadMoreJobs() {
        std::string error;
        const auto rows = project_.list_jobs(100, job_offset_, {}, &error);
        for (const auto& value : rows) {
            const int row = jobs_->rowCount();
            jobs_->insertRow(row);
            const QString phase = value.latest_sequence == 0
                ? QStringLiteral("—")
                : QString::fromLatin1(job_phase_name(value.latest_phase)).replace('_', ' ');
            const QString progress = value.latest_sequence == 0
                ? QStringLiteral("—")
                : value.total_known && value.total > 0
                    ? QStringLiteral("%1/%2").arg(value.completed).arg(value.total)
                    : QStringLiteral("active");
            const std::int64_t elapsed = value.started_at > 0
                ? std::max<std::int64_t>(0,
                    (value.finished_at > 0 ? value.finished_at : now()) - value.started_at)
                : 0;
            const QString heartbeat = value.heartbeat_at > 0
                ? QStringLiteral("%1 · %2s").arg(value.heartbeat_at).arg(elapsed)
                : QStringLiteral("—");
            const QString values[] = {QString::fromStdString(value.job_id),
                QString::fromLatin1(project_job_state_name(value.state)),
                QString::fromStdString(value.operation), phase, progress,
                QString::fromStdString(value.target_id),
                QStringLiteral("%1/%2").arg(value.attempt).arg(value.max_attempts),
                heartbeat,
                QString::fromStdString(value.error_code.empty() ? value.error_message
                                                                 : value.error_code + ": " + value.error_message)};
            for (int col = 0; col < 9; ++col)
                jobs_->setItem(row, col, new QTableWidgetItem(values[col]));
        }
        job_offset_ += rows.size();
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Job query"), QString::fromStdString(error));
    }

    void loadMoreArtifacts() {
        std::string error;
        const auto rows = project_.list_artifacts(100, artifact_offset_, {}, &error);
        for (const auto& value : rows) {
            const int row = artifacts_->rowCount();
            artifacts_->insertRow(row);
            const QString values[] = {QString::fromStdString(value.kind),
                QString::fromStdString(value.digest), QString::number(value.size),
                QString::fromStdString(value.relative_path)};
            for (int col = 0; col < 4; ++col) artifacts_->setItem(row, col, new QTableWidgetItem(values[col]));
        }
        artifact_offset_ += rows.size();
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Artifact query"), QString::fromStdString(error));
    }

    std::int64_t now() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void resetJobs() {
        jobs_->setRowCount(0);
        job_offset_ = 0;
        loadMoreJobs();
        refreshOverview();
    }

    void retrySelectedJob() {
        const int row = jobs_->currentRow();
        if (row < 0 || !jobs_->item(row, 0)) return;
        const std::string id = jobs_->item(row, 0)->text().toStdString();
        if (QMessageBox::question(this, QStringLiteral("Retry failed job"),
                QStringLiteral("Grant one additional attempt to %1?").arg(QString::fromStdString(id)))
            != QMessageBox::Yes) return;
        std::string error;
        if (!project_.retry_failed_job(id, now(), &error))
            QMessageBox::warning(this, QStringLiteral("Retry unavailable"), QString::fromStdString(error));
        else resetJobs();
    }

    void cancelSelectedJob() {
        const int row = jobs_->currentRow();
        if (row < 0 || !jobs_->item(row, 0)) return;
        const std::string id = jobs_->item(row, 0)->text().toStdString();
        if (QMessageBox::question(this, QStringLiteral("Cancel job"),
                QStringLiteral("Request safe cancellation of %1?")
                    .arg(QString::fromStdString(id))) != QMessageBox::Yes)
            return;
        std::string error;
        if (!project_.request_job_cancellation(id, now(), &error))
            QMessageBox::warning(this, QStringLiteral("Cancellation unavailable"),
                                 QString::fromStdString(error));
        else
            resetJobs();
    }

    void showSelectedJob() {
        const int row = jobs_->currentRow();
        if (row < 0 || !jobs_->item(row, 0)) return;
        const std::string id = jobs_->item(row, 0)->text().toStdString();
        std::string error;
        const auto events = project_.list_job_events(id, 0, 1000, &error);
        if (!error.empty()) {
            QMessageBox::warning(this, QStringLiteral("Job event query"),
                                 QString::fromStdString(error));
            return;
        }
        QString detail;
        for (const StoredJobProgressEvent& stored : events) {
            detail += QStringLiteral("#%1  %2  %3/%4  %5\n")
                .arg(stored.event.sequence)
                .arg(QString::fromLatin1(job_phase_name(stored.event.phase)))
                .arg(stored.event.completed).arg(stored.event.total)
                .arg(QString::fromStdString(stored.event.message_code));
        }
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("Job details"));
        box.setText(QString::fromStdString(id));
        box.setInformativeText(QStringLiteral("%1 persisted event(s)").arg(events.size()));
        box.setDetailedText(detail.isEmpty() ? QStringLiteral("No events recorded.") : detail);
        box.exec();
    }

    void recoverStaleJobs() {
        if (QMessageBox::question(this, QStringLiteral("Recover stale jobs"),
                QStringLiteral("Recover running jobs with no heartbeat for 10 minutes?"))
            != QMessageBox::Yes) return;
        std::string error;
        const std::int64_t current = now();
        const std::uint64_t count = project_.recover_abandoned_jobs(current - 600, current, &error);
        if (!error.empty()) QMessageBox::warning(this, QStringLiteral("Recovery failed"), QString::fromStdString(error));
        else QMessageBox::information(this, QStringLiteral("Recovery complete"),
                                      QStringLiteral("Recovered %1 job(s).").arg(count));
        resetJobs();
    }

    std::string resolvedSource() {
        if (!source_path_.empty()) return source_path_;
        std::string error;
        const auto sources = project_.list_sources(1, 0, &error);
        if (!sources.empty()) source_path_ = sources.front().source_path;
        return source_path_;
    }

    bool decisionHistoryAvailable() const {
        if (!log_path_ || log_path_->text().trimmed().isEmpty()) return false;
        std::error_code error;
        const std::filesystem::path path(log_path_->text().toStdString());
        return std::filesystem::is_regular_file(path, error)
            && !error && std::filesystem::file_size(path, error) > 0 && !error;
    }

    enum class PendingGeometryAction { none, open_review, open_autonomy };

    void restoreDecisions() {
        try {
            if (session_ || geometry_load_controller_->active()) return;
            const std::string source = resolvedSource();
            if (source.empty())
                throw std::runtime_error("select the recorded source STEP first");
            const std::string log = log_path_->text().trimmed().toStdString();
            if (!decisionHistoryAvailable())
                throw std::runtime_error("no decision history is available");
            const std::string recipe = recipe_path_->text().trimmed().toStdString();
            if (recipe.empty())
                throw std::runtime_error("choose a recipe output path");
            CompactRecipeOptions options;
            options.preserve_step_exports = false;
            if (compact_log_to_recipe(log, recipe, options) != 0)
                throw std::runtime_error(
                    "the decision history could not be compacted into a typed recipe");
            std::string recipe_json = recipe;
            if (recipe_json.size() > 4
                && recipe_json.compare(recipe_json.size() - 4, 4, ".lua") == 0)
                recipe_json.replace(recipe_json.size() - 4, 4, ".json");
            else
                recipe_json += ".json";
            const std::string replay_report =
                (project_.root() / "reconstruction-report.json").string();
            pending_geometry_action_ = PendingGeometryAction::open_review;
            pending_focus_id_.clear();
            pending_focused_display_ = false;
            geometry_load_succeeded_ = false;
            reconstructing_load_ = true;
            evaluated_readiness_ = QStringLiteral(
                "Reconstructing saved decisions — exact source and preparation identity are required.");
            readiness_->setText(evaluated_readiness_);
            geometry_load_dock_->setWindowTitle(QStringLiteral("Project reconstruction"));
            geometry_load_dock_->beginJob();
            if (!geometry_load_controller_->startDocumentReconstruction(
                    source, log, recipe_json, replay_report, load_options_)) {
                reconstructing_load_ = false;
                pending_geometry_action_ = PendingGeometryAction::none;
                geometry_load_dock_->hide();
                throw std::runtime_error("another model task is already active");
            }
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Restore unavailable"),
                                  QString::fromStdString(error.what()));
        }
    }

    void chooseSource() {
        if (active_geometry_ || active_autonomy_ || agentBusy()) {
            QMessageBox::information(this, QStringLiteral("Close active review"),
                QStringLiteral("Close the geometry and decision-queue review windows "
                               "before replacing the source model."));
            return;
        }
        const QString selected = QFileDialog::getOpenFileName(
            this, QStringLiteral("Choose source STEP model"),
            QString::fromStdString(project_.root().string()),
            QStringLiteral("STEP models (*.step *.stp);;All files (*)"));
        if (selected.isEmpty()) return;
        if (session_ && QMessageBox::question(this,
                QStringLiteral("Replace active model"),
                QStringLiteral("Choosing another source clears the current in-memory "
                               "review session. Project history and files are preserved."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                != QMessageBox::Yes)
            return;
        session_.reset();
        document_id_.clear();
        if (agent_dock_) agent_dock_->setAvailable(false);
        source_path_ = selected.toStdString();
        geometry_load_succeeded_ = false;
        evaluated_readiness_ = QStringLiteral(
            "Source selected — load the model to evaluate readiness.");
        if (log_path_) log_path_->setEnabled(true);
        if (export_step_button_) export_step_button_->setEnabled(false);
        if (export_mcnp_button_) export_mcnp_button_->setEnabled(false);
        refreshOverview();
    }

    void editPolicy() {
        if (active_geometry_ || active_autonomy_ || agentBusy()) {
            QMessageBox::information(this, QStringLiteral("Close active review"),
                QStringLiteral("Close the geometry and decision-queue review windows "
                               "before changing project authority."));
            return;
        }
        ProjectPolicyDialog dialog(policy_, this);
        if (dialog.exec() != QDialog::Accepted) return;
        if (session_ && QMessageBox::question(this,
                QStringLiteral("Save policy and unload model"),
                QStringLiteral("The active model was loaded under the previous policy. "
                               "Save this policy and unload it so evidence can be regenerated?"),
                QMessageBox::Save | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Save)
            return;
        if (write_autonomy_project_policy_json(policy_path_, dialog.result()) != 0) {
            QMessageBox::critical(this, QStringLiteral("Policy save failed"),
                QStringLiteral("The complete policy could not be written atomically. "
                               "The previous policy remains active."));
            return;
        }
        try {
            policy_ = read_autonomy_project_policy_json(policy_path_);
            load_options_ = conversionOptionsForProject(policy_);
            session_.reset();
            document_id_.clear();
            if (agent_dock_) agent_dock_->setAvailable(false);
            queue_.reset();
            geometry_load_succeeded_ = false;
            evaluated_readiness_ = QStringLiteral(
                "Policy saved — reload the source to regenerate geometry evidence. "
                "Attached decision queues must be re-evaluated against the new policy.");
            if (log_path_) log_path_->setEnabled(true);
            if (export_step_button_) export_step_button_->setEnabled(false);
            if (export_mcnp_button_) export_mcnp_button_->setEnabled(false);
            refreshOverview();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Saved policy invalid"),
                                  QString::fromStdString(error.what()));
        }
    }

    void reloadPolicy() {
        if (active_geometry_ || active_autonomy_ || agentBusy()) {
            QMessageBox::information(this, QStringLiteral("Close active review"),
                QStringLiteral("Close the geometry and decision-queue review windows "
                               "before reloading project authority."));
            return;
        }
        try {
            policy_ = read_autonomy_project_policy_json(policy_path_);
            load_options_ = conversionOptionsForProject(policy_);
            if (session_) {
                session_.reset();
                document_id_.clear();
                if (agent_dock_) agent_dock_->setAvailable(false);
                queue_.reset();
                geometry_load_succeeded_ = false;
                evaluated_readiness_ = QStringLiteral(
                    "Policy reloaded — reload the source model before review or export.");
                if (log_path_) log_path_->setEnabled(true);
                if (export_step_button_) export_step_button_->setEnabled(false);
                if (export_mcnp_button_) export_mcnp_button_->setEnabled(false);
            }
            refreshOverview();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Project policy invalid"),
                                  QString::fromStdString(error.what()));
        }
    }

    void resetProjectIndexes() {
        definitions_->setRowCount(0);
        instances_->setRowCount(0);
        spatial_->setRowCount(0);
        definition_offset_ = instance_offset_ = spatial_offset_ = 0;
        loadMoreDefinitions();
        loadMoreInstances();
        spatial_namespace_->clear();
        std::string error;
        for (const std::string& value : project_.list_spatial_namespaces(&error))
            spatial_namespace_->addItem(QString::fromStdString(value));
        loadSpatial(true);
        refreshOverview();
    }

    void startGeometryLoad(PendingGeometryAction action,
                           std::string focus_id = {}, bool focused = false) {
        if (session_) return;
        if (geometry_load_controller_->active()) {
            geometry_load_dock_->show();
            geometry_load_dock_->raise();
            return;
        }
        const std::string source = resolvedSource();
        if (source.empty()) throw std::runtime_error("no source STEP was provided or indexed");
        pending_geometry_action_ = action;
        reconstructing_load_ = false;
        pending_focus_id_ = std::move(focus_id);
        pending_focused_display_ = focused;
        geometry_load_succeeded_ = false;
        evaluated_readiness_ = QStringLiteral(
            "Loading and analyzing model — the workspace remains available.");
        if (readiness_) readiness_->setText(evaluated_readiness_);
        geometry_load_dock_->beginJob();
        const std::string log = log_path_ ? log_path_->text().toStdString()
                                          : std::string();
        if (!geometry_load_controller_->startDocumentLoad(
                source, log, load_options_)) {
            pending_geometry_action_ = PendingGeometryAction::none;
            geometry_load_dock_->hide();
            throw std::runtime_error("another model task is already active");
        }
    }

    void startExport(ExportKind kind) {
        if (!session_ || geometry_load_controller_->active()) return;
        const bool step = kind == ExportKind::step;
        const QString suggested = QString::fromStdString(
            (project_.root() / (step ? "model-reviewed.step" : "model.i")).string());
        const QString selected = QFileDialog::getSaveFileName(
            this, step ? QStringLiteral("Export reviewed STEP")
                       : QStringLiteral("Export MCNP geometry"),
            suggested, step ? QStringLiteral("STEP models (*.step *.stp)")
                            : QStringLiteral("MCNP input (*.i *.inp);;All files (*)"));
        if (selected.isEmpty()) return;
        export_kind_ = kind;
        export_path_ = selected.toStdString();
        geometry_load_succeeded_ = true;
        evaluated_readiness_ = step
            ? QStringLiteral("Exporting reviewed STEP…")
            : QStringLiteral("Validating and exporting MCNP…");
        readiness_->setText(evaluated_readiness_);
        geometry_load_dock_->setWindowTitle(step
            ? QStringLiteral("STEP export") : QStringLiteral("MCNP export"));
        geometry_load_dock_->beginJob();
        const bool started = step
            ? geometry_load_controller_->startStepExport(
                *session_, document_id_, export_path_)
            : geometry_load_controller_->startEmission(
                *session_, document_id_, export_path_);
        if (!started) {
            export_kind_ = ExportKind::none;
            export_path_.clear();
            geometry_load_dock_->hide();
        }
    }

    void continuePendingGeometryAction() {
        if (close_after_geometry_load_) {
            pending_geometry_action_ = PendingGeometryAction::none;
            return;
        }
        const PendingGeometryAction action = pending_geometry_action_;
        const std::string focus = std::move(pending_focus_id_);
        const bool focused = pending_focused_display_;
        pending_geometry_action_ = PendingGeometryAction::none;
        pending_focused_display_ = false;
        if (action == PendingGeometryAction::open_review)
            openGeometry(focus, focused);
        else if (action == PendingGeometryAction::open_autonomy)
            openAutonomy();
    }

    void openGeometry(const std::string& focus_id = {}, bool focused = false) {
        try {
            if (!session_) {
                startGeometryLoad(PendingGeometryAction::open_review,
                                  focus_id, focused);
                return;
            }
            if (active_geometry_) { active_geometry_->raise(); return; }
            auto* review = new DecisionWindow(*session_, resolvedSource(), document_id_,
                focus_id, nullptr, std::string(), ViewerReviewContext{}, std::string(),
                {}, focused, &project_);
            review->setAttribute(Qt::WA_DeleteOnClose);
            active_geometry_ = review;
            connect(review, &QObject::destroyed, this, [this]() { active_geometry_.clear(); });
            review->show();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Geometry load failed"), QString::fromStdString(error.what()));
        }
    }

    void resumePendingReview() {
        try {
            if (!session_) {
                throw std::runtime_error(
                    "restore or load the project session before resuming review");
            }
            if (active_geometry_) {
                active_geometry_->raise();
                return;
            }
            const std::vector<DecisionReviewRecord> pending =
                latest_decision_reviews(*session_, "pending");
            if (pending.empty()) {
                throw std::runtime_error("no pending review is available");
            }
            const DecisionReviewRecord& saved = pending.back();
            ViewerReviewContext review;
            review.kind = saved.review_kind;
            review.review_id = saved.proposal_id;
            review.action = saved.action;
            review.target_id = saved.target_id;
            review.candidate_id = saved.candidate_id;
            review.audit = saved.audit;
            const DecompositionProposal* proposal = nullptr;
            std::string preselect = saved.target_id;
            resumed_review_proposal_ = {};
            if (saved.action == "apply_decomposition_candidate") {
                resumed_review_proposal_ = propose_decomposition(
                    *session_, saved.target_id);
                const DecompositionCandidate* candidate = find_candidate(
                    &resumed_review_proposal_, saved.candidate_id);
                if (!candidate) {
                    throw std::runtime_error(
                        "the saved decomposition candidate is no longer present; "
                        "generate new options from the geometry workspace");
                }
                review.kind = "decomposition";
                proposal = &resumed_review_proposal_;
            } else if (saved.action == "remove_solid") {
                review.kind = "removal";
            } else if (saved.action
                       == "apply_surface_replacement_candidate") {
                review.kind = "surface_replacement";
                const SurfaceReplacementProposal surface =
                    propose_surface_replacement(*session_, saved.target_id);
                const auto candidate = std::find_if(
                    surface.candidates.begin(), surface.candidates.end(),
                    [&](const SurfaceReplacementCandidate& value) {
                        return value.id == saved.candidate_id;
                    });
                if (candidate == surface.candidates.end()) {
                    throw std::runtime_error(
                        "the saved surface replacement is no longer present; "
                        "regenerate surface options from the geometry workspace");
                }
                review.surface_replacement = *candidate;
                review.has_surface_replacement = true;
                preselect = candidate->owner_target_id;
            } else if (saved.action == "fill_hole") {
                review.kind = "hole_fill";
                const HoleFillProposal holes = propose_hole_fills(
                    *session_, document_id_, HoleFillOptions{});
                const auto candidate = std::find_if(
                    holes.candidates.begin(), holes.candidates.end(),
                    [&](const HoleFillCandidate& value) {
                        return value.id == saved.candidate_id;
                    });
                if (candidate == holes.candidates.end()) {
                    throw std::runtime_error(
                        "the saved hole-fill candidate is no longer present; "
                        "regenerate hole-fill options from the geometry workspace");
                }
                review.hole_fill = *candidate;
                review.has_hole_fill = true;
                preselect = candidate->host_solid_id;
            } else if (saved.action == "apply_contained_medium") {
                review.kind = "contained_medium";
                if (!find_contained_medium_request(
                        *session_, document_id_, saved.candidate_id,
                        &review.medium_request)) {
                    throw std::runtime_error(
                        "the staged contained-medium request is unavailable; "
                        "restore or restage the request before review");
                }
                review.medium_proposal = propose_contained_medium(
                    *session_, document_id_, review.medium_request);
                if (!review.medium_proposal.ok) {
                    throw std::runtime_error(
                        "the contained-medium certificate no longer passes; "
                        "edit the staged request and regenerate its evidence");
                }
                review.has_medium = true;
                preselect = review.medium_proposal.host_shape_id;
            } else {
                throw std::runtime_error(
                    "this pending operation has no restart review adapter: "
                    + saved.action);
            }
            auto* window = new DecisionWindow(*session_, resolvedSource(),
                document_id_, preselect, proposal, saved.candidate_id,
                review, std::string(), {}, true, &project_);
            window->setAttribute(Qt::WA_DeleteOnClose);
            active_geometry_ = window;
            connect(window, &QObject::destroyed, this, [this]() {
                active_geometry_.clear();
                refreshOverview();
            });
            window->show();
        } catch (const std::exception& error) {
            QMessageBox::warning(this, QStringLiteral("Pending review unavailable"),
                QString::fromStdString(error.what()));
        }
    }

    void openCoverage() {
        try {
            if (coverage_path_.empty()) throw std::runtime_error("no coverage certificate path was provided");
            auto certificate = read_alea_coverage_certificate_json(coverage_path_);
            auto* review = new CoverageReviewWindow(std::move(certificate), coverage_path_);
            review->setAttribute(Qt::WA_DeleteOnClose);
            review->show();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Coverage review failed"), QString::fromStdString(error.what()));
        }
    }

    void openAutonomy() {
        try {
            if (active_autonomy_) { active_autonomy_->raise(); return; }
            if (queue_path_.empty()) throw std::runtime_error("no autonomy queue path was provided");
            if (!session_) {
                startGeometryLoad(PendingGeometryAction::open_autonomy);
                return;
            }
            queue_ = std::make_unique<AutonomyQueue>(read_autonomy_queue_json(queue_path_));
            auto* review = new AutonomyQueueWindow(*session_, resolvedSource(),
                *queue_, queue_path_, &project_);
            review->setAttribute(Qt::WA_DeleteOnClose);
            active_autonomy_ = review;
            connect(review, &QObject::destroyed, this, [this]() {
                active_autonomy_.clear();
                jobs_->setRowCount(0);
                job_offset_ = 0;
                loadMoreJobs();
                artifacts_->setRowCount(0);
                artifact_offset_ = 0;
                loadMoreArtifacts();
                refreshReviewState();
            });
            review->show();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Decision queue failed"), QString::fromStdString(error.what()));
        }
    }

    void refreshReviewState() {
        std::ostringstream text;
        text << "Coverage certificate: " << (coverage_path_.empty() ? "not attached" : coverage_path_) << '\n';
        if (!coverage_path_.empty()) {
            try {
                const auto certificate = read_alea_coverage_certificate_json(coverage_path_);
                text << "  status=" << alea_coverage_status_name(certificate.status)
                     << ", gaps=" << certificate.coverage.gap_count
                     << ", overlaps=" << certificate.coverage.overlap_count
                     << ", unresolved=" << certificate.coverage.unresolved_count << '\n';
            } catch (const std::exception& error) { text << "  invalid: " << error.what() << '\n'; }
        }
        text << "Autonomy queue: " << (queue_path_.empty() ? "not attached" : queue_path_) << '\n';
        if (!queue_path_.empty()) {
            try {
                auto queue = read_autonomy_queue_json(queue_path_);
                refresh_autonomy_queue_counts(queue);
                text << "  applied=" << queue.applied_count << ", review=" << queue.human_review_count
                     << ", denied=" << queue.denied_count << ", failed=" << queue.failed_count << '\n';
            } catch (const std::exception& error) { text << "  invalid: " << error.what() << '\n'; }
        }
        review_state_->setPlainText(QString::fromStdString(text.str()));
        if (coverage_button_) {
            coverage_button_->setEnabled(!coverage_path_.empty());
            coverage_button_->setToolTip(coverage_path_.empty()
                ? QStringLiteral("No coverage certificate is attached.") : QString());
        }
        if (autonomy_button_) {
            autonomy_button_->setEnabled(!queue_path_.empty());
            autonomy_button_->setToolTip(queue_path_.empty()
                ? QStringLiteral("No autonomy queue is attached.") : QString());
        }
    }

    void compactRecipe() {
        CompactRecipeOptions options;
        options.preserve_step_exports = preserve_steps_->isChecked();
        const std::string log = log_path_->text().toStdString();
        const std::string recipe = recipe_path_->text().toStdString();
        if (compact_log_to_recipe(log, recipe, options) != 0)
            QMessageBox::critical(this, QStringLiteral("Compaction failed"), QStringLiteral("Could not compact the selected decision log."));
        else QMessageBox::information(this, QStringLiteral("Recipe written"), QString::fromStdString(recipe));
    }

    ProjectStore project_;
    std::string source_path_, coverage_path_, queue_path_, document_id_;
    AutonomyProjectPolicy policy_;
    ConversionOptions load_options_;
    std::string policy_path_;
    QLabel *readiness_ = nullptr, *overview_ = nullptr,
           *source_summary_ = nullptr, *policy_summary_ = nullptr;
    QTableWidget *definitions_ = nullptr, *instances_ = nullptr,
                 *spatial_ = nullptr, *jobs_ = nullptr, *artifacts_ = nullptr;
    QPlainTextEdit* review_state_ = nullptr;
    QLineEdit *log_path_ = nullptr, *recipe_path_ = nullptr,
              *assembly_search_ = nullptr, *spatial_search_ = nullptr;
    QComboBox *spatial_namespace_ = nullptr, *spatial_kind_ = nullptr;
    QCheckBox* preserve_steps_ = nullptr;
    QPushButton *geometry_button_ = nullptr, *coverage_button_ = nullptr,
                *autonomy_button_ = nullptr, *choose_source_button_ = nullptr,
                *edit_policy_button_ = nullptr,
                *restore_button_ = nullptr,
                *resume_review_button_ = nullptr,
                *export_step_button_ = nullptr,
                *export_mcnp_button_ = nullptr;
    std::size_t definition_offset_ = 0, instance_offset_ = 0,
                spatial_offset_ = 0, job_offset_ = 0, artifact_offset_ = 0;
    SessionPtr session_;
    std::unique_ptr<AutonomyQueue> queue_;
    DecompositionProposal resumed_review_proposal_;
    QPointer<DecisionWindow> active_geometry_;
    QPointer<AutonomyQueueWindow> active_autonomy_;
    CadJobController* geometry_load_controller_ = nullptr;
    CadJobController* agent_controller_ = nullptr;
#if CSGIFY_HAVE_OPENAI_PROVIDER
    OpenAiAgentProvider* openai_agent_provider_ = nullptr;
#endif
    JobProgressDock* geometry_load_dock_ = nullptr;
    AgentConversationDock* agent_dock_ = nullptr;
    std::vector<AgentMessage> agent_messages_;
    AgentContextSnapshotPtr pending_agent_snapshot_;
    QString pending_agent_question_;
    const std::string agent_conversation_id_ = "conversation:project-default";
    std::string pending_agent_user_message_id_;
    std::string last_agent_message_id_;
    std::uint64_t agent_message_counter_ = 0;
    PendingGeometryAction pending_geometry_action_ = PendingGeometryAction::none;
    std::string pending_focus_id_;
    bool pending_focused_display_ = false;
    bool geometry_load_succeeded_ = false;
    bool reconstructing_load_ = false;
    bool close_after_geometry_load_ = false;
    bool close_after_agent_ = false;
    ExportKind export_kind_ = ExportKind::none;
    std::string export_path_;
    QString evaluated_readiness_;
};

class ProjectLauncherWindow final : public QMainWindow {
public:
    ProjectLauncherWindow() {
        setWindowTitle(QStringLiteral("csgify — Projects"));
        resize(560, 300);
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        auto* title = new QLabel(QStringLiteral("Geometry workspace"), page);
        QFont title_font = title->font();
        title_font.setBold(true);
        title_font.setPointSize(title_font.pointSize() + 5);
        title->setFont(title_font);
        layout->addWidget(title);
        auto* description = new QLabel(QStringLiteral(
            "Create a project to keep source identity, geometry evidence, decisions, "
            "jobs, and exports together. Opening a project does not load CAD until "
            "you ask the workspace to evaluate it."), page);
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addSpacing(18);
        auto* create = new QPushButton(QStringLiteral("New project…"), page);
        auto* open = new QPushButton(QStringLiteral("Open project…"), page);
        create->setObjectName(QStringLiteral("newProjectButton"));
        open->setObjectName(QStringLiteral("openProjectButton"));
        create->setMinimumHeight(44);
        open->setMinimumHeight(44);
        layout->addWidget(create);
        layout->addWidget(open);
        layout->addStretch();
        setCentralWidget(page);
        connect(create, &QPushButton::clicked, this, [this]() { createProject(); });
        connect(open, &QPushButton::clicked, this, [this]() { openProject(); });
    }

private:
    void createProject() {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose or create a project folder"));
        if (directory.isEmpty()) return;
        try {
            ProjectStore project(directory.toStdString());
            std::string error;
            if (!project.open(&error)) throw std::runtime_error(error);
            const std::filesystem::path policy_path =
                project.root() / "project-policy.json";
            std::error_code policy_error;
            if (!std::filesystem::exists(policy_path, policy_error)) {
                std::string project_id = project.root().filename().string();
                if (project_id.empty()) project_id = "csgify-project";
                if (policy_error || write_default_autonomy_project_policy_json(
                        policy_path.string(), project_id) != 0)
                    throw std::runtime_error("could not create the project policy");
            } else {
                (void)read_autonomy_project_policy_json(policy_path.string());
            }
            showWorkspace(project.root().string());
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Project creation failed"),
                                  QString::fromStdString(error.what()));
        }
    }

    void openProject() {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Open csgify project"));
        if (directory.isEmpty()) return;
        try {
            ProjectStore project(directory.toStdString());
            std::error_code probe_error;
            if (!std::filesystem::is_regular_file(
                    project.database_path(), probe_error))
                throw std::runtime_error(
                    "This folder is not an initialized csgify project.");
            std::string error;
            if (!project.open(&error)) throw std::runtime_error(error);
            showWorkspace(project.root().string());
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Project open failed"),
                                  QString::fromStdString(error.what()));
        }
    }

    void showWorkspace(const std::string& project_path) {
        auto* workspace = new ProjectWorkspaceWindow(
            project_path, {}, {}, {});
        workspace->setAttribute(Qt::WA_DeleteOnClose);
        connect(workspace, &QObject::destroyed, this, [this]() {
            show();
            raise();
        });
        hide();
        workspace->show();
    }
};

} // namespace

int run_qt_viewer_command(Session& session,
                          const std::string& step_path,
                          const std::string& document_id,
                          const std::string& preselect_id,
                          const DecompositionProposal* proposal,
                          const std::string& candidate_id,
                          const ViewerReviewContext& review,
                          const std::string& commit_out_path) {
#if defined(Q_OS_LINUX)
    // OCCT's TKOpenGL backend wraps an X11 native window. Under a Wayland
    // desktop, use the XWayland Qt platform so QWidget::winId() is an XID.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
        && std::getenv("DISPLAY") != nullptr) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
    }
#endif
    int argc = 1;
    char app_name[] = "csgify";
    char* argv[] = {app_name, nullptr};
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("csgify"));
    application.setOrganizationName(QStringLiteral("csgify"));
    DecisionWindow window(session, step_path, document_id, preselect_id,
                          proposal, candidate_id, review, commit_out_path);
    window.show();
    return application.exec();
}

int run_qt_decision_widget_smoke(Session& session,
                                 const std::string& step_path,
                                 const std::string& document_id) {
    int argc = 1;
    char app_name[] = "csgify-qt-widget-smoke";
    char* argv[] = {app_name, nullptr};
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("csgify Qt widget smoke"));

    DecisionWindow window(session, step_path, document_id, {}, nullptr, {},
                          ViewerReviewContext{}, {});
    const auto fail = [](const char* message) {
        std::cerr << "qt_widget_smoke: " << message << '\n';
        return EXIT_FAILURE;
    };
    const auto wait_until = [&application](const std::function<bool()>& ready,
                                            int timeout_ms) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!ready() && elapsed.elapsed() < timeout_ms) {
            application.processEvents(QEventLoop::AllEvents, 25);
            QThread::msleep(2);
        }
        application.processEvents(QEventLoop::AllEvents, 25);
        return ready();
    };

    // Opt-in native render check; requires a real desktop session.
    const QString native_capture = qEnvironmentVariable("CSGIFY_NATIVE_VIEW_CAPTURE");
    if (!native_capture.isEmpty()) {
        window.show();
        application.processEvents();
        auto* viewport = dynamic_cast<OcctViewWidget*>(window.findChild<QWidget*>(
            QStringLiteral("nativeGeometryViewport")));
        if (!viewport) return fail("native viewport not found");
        window.resize(1100, 760);
        application.processEvents();
        if (!viewport->dumpPng(native_capture))
            return fail("native viewport capture failed");
        window.close();
        return EXIT_SUCCESS;
    }

    // Optional real-widget layout captures. Keep the native OCCT surface
    // hidden: offscreen Qt has no X11/OpenGL drawable for it.
    const QString capture_directory = qEnvironmentVariable("CSGIFY_WIDGET_CAPTURE_DIR");
    QLabel* viewport_placeholder = nullptr;
    if (!capture_directory.isEmpty()) {
        auto* viewport = window.findChild<QWidget*>(
            QStringLiteral("nativeGeometryViewport"));
        viewport->hide();
        viewport_placeholder = new QLabel(QStringLiteral(
            "3D geometry viewport\nNative OCCT rendering omitted in this layout capture"),
            viewport->parentWidget());
        viewport_placeholder->setAlignment(Qt::AlignCenter);
        viewport_placeholder->setMinimumSize(280, 240);
        viewport_placeholder->setStyleSheet(QStringLiteral(
            "background: #263b48; color: #dce6ec; padding: 16px;"));
        auto* layout = qobject_cast<QVBoxLayout*>(viewport->parentWidget()->layout());
        layout->insertWidget(layout->indexOf(viewport), viewport_placeholder, 1);
        if (!QDir().mkpath(capture_directory))
            return fail("could not create widget capture directory");
        window.show();
        application.processEvents();
    }
    const auto capture = [&](const QString& name, const QSize& size) {
        if (!viewport_placeholder) return true;
        window.resize(size);
        application.processEvents();
        return window.grab().save(capture_directory + QLatin1Char('/') + name);
    };

    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("modelStatusTree"));
    auto* summary = window.findChild<QLabel*>(QStringLiteral("decisionSummary"));
    auto* resolve = window.findChild<QPushButton*>(
        QStringLiteral("resolveCertifiedShapesButton"));
    auto* generate = window.findChild<QPushButton*>(
        QStringLiteral("generateDecompositionOptionsButton"));
    auto* candidates = window.findChild<QTableWidget*>(
        QStringLiteral("proposalCandidatesTable"));
    auto* accept = window.findChild<QPushButton*>(
        QStringLiteral("acceptProposalButton"));
    auto* preview = window.findChild<QRadioButton*>(
        QStringLiteral("previewGeometryButton"));
    auto* cancel = window.findChild<QPushButton*>(
        QStringLiteral("cancelGeometryJobButton"));
    auto* grid = window.findChild<QPushButton*>(
        QStringLiteral("referenceGridButton"));
    auto* axes = window.findChild<QPushButton*>(
        QStringLiteral("referenceAxesButton"));
    auto* accept_shortcut = window.findChild<QShortcut*>(
        QStringLiteral("acceptProposalShortcut"));
    auto* controller = window.findChild<CadJobController*>();
    auto* search = window.findChild<QLineEdit*>(QStringLiteral("modelSearchField"));
    auto* evidence = window.findChild<QTextBrowser*>(
        QStringLiteral("geometryEvidenceBrowser"));
    if (!tree || !summary || !resolve || !generate || !candidates || !accept
        || !preview || !cancel || !grid || !axes || !accept_shortcut
        || !controller || !search || !evidence) {
        return fail("required decision widgets were not constructed");
    }
    if (!resolve->text().contains(QStringLiteral("unresolved"))
        || !resolve->isEnabled() || generate->isEnabled()) {
        return fail("inspection actions do not reflect the initial selection state");
    }
    if (!grid->isChecked() || !axes->isChecked() || accept_shortcut->isEnabled())
        return fail("initial viewport toggles or guarded shortcut state is wrong");
    grid->click();
    axes->click();
    if (grid->isChecked() || axes->isChecked())
        return fail("viewport reference toggles did not respond to interaction");
    grid->click();
    axes->click();
    QTreeWidgetItem* root = tree->topLevelItem(0);
    if (!root || root->childCount() == 0)
        return fail("model tree has no selectable shape");
    tree->setCurrentItem(root->child(0));
    application.processEvents();
    if (!generate->isEnabled()
        || !summary->text().contains(QStringLiteral("Needs decomposition"))) {
        return fail("selecting an unresolved shape did not expose its action");
    }
    if (!evidence->toPlainText().contains(QStringLiteral("Selected shape"))
        || !evidence->toPlainText().contains(QStringLiteral("Recommended:")))
        return fail("inspection evidence lost the selected shape and recommendation");

    const QString selected_shape = root->child(0)->data(0, Qt::UserRole).toString();
    search->setText(QStringLiteral("no-such-component"));
    application.processEvents();
    if (!root->isHidden() || !generate->isEnabled()
        || tree->currentItem()->data(0, Qt::UserRole).toString() != selected_shape)
        return fail("search changed selection or left unrelated shapes visible");
    search->setText(QStringLiteral("Needs decomposition"));
    application.processEvents();
    if (root->isHidden() || root->child(0)->isHidden())
        return fail("status search did not preserve matching shape ancestors");
    search->clear();
    if (!capture(QStringLiteral("inspection.png"), QSize(1500, 900))
        || !capture(QStringLiteral("inspection-small.png"), QSize(1100, 720)))
        return fail("could not capture inspection layout");

    generate->click();
    if (generate->isEnabled())
        return fail("candidate generation did not enter its busy state");
    QCloseEvent generating_close;
    application.sendEvent(&window, &generating_close);
    if (generating_close.isAccepted())
        return fail("close was accepted while candidate generation was active");
    if (!wait_until([candidates]() { return candidates->rowCount() > 0; }, 30000))
        return fail("candidate generation did not complete");
    if (candidates->isHidden() || !accept->isEnabled() || !preview->isEnabled()
        || !accept_shortcut->isEnabled())
        return fail("completed candidates did not enter review state");

    if (!capture(QStringLiteral("review.png"), QSize(1500, 900))
        || !capture(QStringLiteral("review-small.png"), QSize(1100, 720)))
        return fail("could not capture review layout");

    preview->click();
    if (!controller->active())
        return fail("preview did not start through the asynchronous controller");
    QCloseEvent preview_close;
    application.sendEvent(&window, &preview_close);
    if (preview_close.isAccepted())
        return fail("close was accepted while preview certification was active");
    cancel->click();
    if (!wait_until([controller]() { return !controller->active(); }, 30000))
        return fail("cancelled preview did not reach a safe terminal boundary");

    QCloseEvent safe_close;
    application.sendEvent(&window, &safe_close);
    if (!safe_close.isAccepted())
        return fail("close remained blocked after the worker stopped");

    AutonomyProposal agent_proposal;
    agent_proposal.id = "widget-agent-proposal";
    agent_proposal.actor = "llm:widget-smoke";
    agent_proposal.op = "apply_decomposition_candidate";
    agent_proposal.target_id = "solid:1";
    agent_proposal.candidate_id = "best";
    agent_proposal.rationale =
        "Explain a bounded decomposition; <b>metadata is data</b>.";
    AutonomyPolicy policy;
    policy.mode = AutonomyMode::propose;
    AutonomyQueue queue = evaluate_autonomy_queue(
        session, document_id, {agent_proposal}, policy);
    if (queue.items.size() != 1
        || queue.items.front().disposition != AutonomyDisposition::human_review) {
        return fail("typed agent proposal did not enter human review");
    }
    {
        AutonomyQueueWindow agent_window(session, step_path, queue, {});
        auto* authority = agent_window.findChild<QLabel*>(
            QStringLiteral("agentAuthoritySummary"));
        auto* proposal_table = agent_window.findChild<QTableWidget*>(
            QStringLiteral("agentProposalTable"));
        auto* explanation = agent_window.findChild<QTextBrowser*>(
            QStringLiteral("agentProposalExplanation"));
        auto* review = agent_window.findChild<QPushButton*>(
            QStringLiteral("reviewAgentProposalButton"));
        if (!authority || !proposal_table || !explanation || !review)
            return fail("agent explanation widgets were not constructed");
        const QString text = explanation->toPlainText();
        if (!authority->text().contains(QStringLiteral("Assisted mode"))
            || !text.contains(QStringLiteral("Agent explanation"))
            || !text.contains(QStringLiteral("Engine evidence"))
            || !text.contains(QStringLiteral("Policy result"))
            || !text.contains(QStringLiteral("llm:widget-smoke"))
            || !text.contains(QStringLiteral("<b>metadata is data</b>"))
            || !review->isEnabled()) {
            return fail("fresh agent proposal is not explained or reviewable");
        }
    }

    AutonomyProposal fixed_point_proposal;
    fixed_point_proposal.id = "widget-fixed-point";
    fixed_point_proposal.op = "decompose_to_fixed_point";
    fixed_point_proposal.target_id = document_id;
    AutonomyProposal shatter_proposal;
    shatter_proposal.id = "widget-budgeted-shatter";
    shatter_proposal.op = "shatter_with_approximation";
    shatter_proposal.target_id = "solid:1";
    AutonomyQueue recovery_queue = evaluate_autonomy_queue(
        session, document_id, {fixed_point_proposal, shatter_proposal}, policy);
    {
        AutonomyQueueWindow recovery_window(
            session, step_path, recovery_queue, {});
        auto* proposal_table = recovery_window.findChild<QTableWidget*>(
            QStringLiteral("agentProposalTable"));
        auto* review = recovery_window.findChild<QPushButton*>(
            QStringLiteral("reviewAgentProposalButton"));
        if (!proposal_table || !review || proposal_table->rowCount() != 2)
            return fail("transactional recovery routes were not constructed");
        proposal_table->selectRow(0);
        application.processEvents();
        if (!review->isEnabled()
            || !review->text().contains(QStringLiteral("Review and run"))) {
            return fail("certified fixed-point review route is unavailable");
        }
        proposal_table->selectRow(1);
        application.processEvents();
        if (!review->isEnabled()
            || !review->text().contains(QStringLiteral("recovery steps"))
            || !review->toolTip().contains(QStringLiteral("shatter"),
                                            Qt::CaseInsensitive)) {
            return fail("policy-blocked shatter lacks an actionable recovery route");
        }
    }

    queue.items.front().evaluated_revision =
        document_revision(session, document_id) + 1;
    {
        AutonomyQueueWindow stale_window(session, step_path, queue, {});
        auto* proposal_table = stale_window.findChild<QTableWidget*>(
            QStringLiteral("agentProposalTable"));
        auto* explanation = stale_window.findChild<QTextBrowser*>(
            QStringLiteral("agentProposalExplanation"));
        auto* review = stale_window.findChild<QPushButton*>(
            QStringLiteral("reviewAgentProposalButton"));
        auto* reevaluate = stale_window.findChild<QPushButton*>(
            QStringLiteral("reevaluateAgentEvidenceButton"));
        if (!proposal_table || !explanation || !review || !reevaluate
            || !proposal_table->item(0, 3)
            || !proposal_table->item(0, 3)->text().contains(QStringLiteral("stale"))
            || !explanation->toPlainText().contains(QStringLiteral("STALE"))
            || review->isEnabled() || !reevaluate->isEnabled()) {
            return fail("stale agent evidence was not visibly blocked");
        }
        reevaluate->click();
        if (reevaluate->isEnabled())
            return fail("agent evidence re-evaluation did not enter busy state");
        QCloseEvent reevaluation_close;
        application.sendEvent(&stale_window, &reevaluation_close);
        if (reevaluation_close.isAccepted())
            return fail("close was accepted while agent evidence was regenerating");
        if (!wait_until([proposal_table, reevaluate]() {
                return reevaluate->isEnabled() && proposal_table->item(0, 3)
                    && !proposal_table->item(0, 3)->text().contains(
                        QStringLiteral("stale"));
            }, 30000)) {
            return fail("agent evidence re-evaluation did not complete");
        }
        if (!review->isEnabled())
            return fail("freshly re-evaluated proposal did not return to review");
    }
    return EXIT_SUCCESS;
}

int run_qt_autonomy_queue(Session& session,
                          const std::string& step_path,
                          AutonomyQueue& queue,
                          const std::string& queue_path) {
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
        && std::getenv("DISPLAY") != nullptr)
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
#endif
    int argc = 1;
    char app_name[] = "csgify";
    char* argv[] = {app_name, nullptr};
    QApplication application(argc, argv);
    AutonomyQueueWindow window(session, step_path, queue, queue_path);
    window.show();
    return application.exec();
}

int run_qt_coverage_review(const AleaCoverageCertificate& certificate,
                           const std::string& certificate_path) {
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
        && std::getenv("DISPLAY") != nullptr)
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
#endif
    int argc = 1;
    char app_name[] = "csgify";
    char* argv[] = {app_name, nullptr};
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("csgify"));
    application.setOrganizationName(QStringLiteral("csgify"));
    CoverageReviewWindow window(certificate, certificate_path);
    window.show();
    return application.exec();
}

int run_qt_project_workspace(const std::string& project_path,
                             const std::string& source_path,
                             const std::string& coverage_path,
                             const std::string& queue_path,
                             bool smoke_test) {
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
        && std::getenv("DISPLAY") != nullptr)
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
#endif
    int argc = 1;
    char app_name[] = "csgify";
    char* argv[] = {app_name, nullptr};
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("csgify"));
    application.setOrganizationName(QStringLiteral("csgify"));
    try {
        ProjectWorkspaceWindow window(project_path, source_path, coverage_path, queue_path);
        if (smoke_test) {
            const bool prototype_controls_present =
                window.findChild<QPushButton*>(
                    QStringLiteral("chooseProjectSourceButton"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("editProjectPolicyButton"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("restoreProjectDecisionsButton"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("resumePendingProjectReviewButton"))
                && window.findChild<QLabel*>(
                    QStringLiteral("projectPolicySummary"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("exportProjectStepButton"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("exportProjectMcnpButton"));
            const bool conversation_controls_present =
                window.findChild<AgentConversationDock*>(
                    QStringLiteral("agentConversationDock"))
                && window.findChild<QPlainTextEdit*>(
                    QStringLiteral("agentQuestionComposer"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("askAgentButton"))
                && window.findChild<QPushButton*>(
                    QStringLiteral("stopAgentButton"));
            const bool embedded_chat_enabled =
                qEnvironmentVariable("CSGIFY_ENABLE_EMBEDDED_CHAT") == QStringLiteral("1");
            const bool any_conversation_control =
                window.findChild<AgentConversationDock*>(QStringLiteral("agentConversationDock"))
                || window.findChild<QPlainTextEdit*>(QStringLiteral("agentQuestionComposer"))
                || window.findChild<QPushButton*>(QStringLiteral("askAgentButton"))
                || window.findChild<QPushButton*>(QStringLiteral("stopAgentButton"));
            if (!prototype_controls_present
                || (embedded_chat_enabled ? !conversation_controls_present
                                          : any_conversation_control))
                return EXIT_FAILURE;
        }
        window.show();
        if (smoke_test) QTimer::singleShot(0, &application, &QCoreApplication::quit);
        return application.exec();
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, QStringLiteral("Project workspace failed"),
                              QString::fromStdString(error.what()));
        return EXIT_FAILURE;
    }
}

int run_qt_project_launcher(bool smoke_test) {
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
        && std::getenv("DISPLAY") != nullptr)
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
#endif
    int argc = 1;
    char app_name[] = "csgify";
    char* argv[] = {app_name, nullptr};
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("csgify"));
    application.setOrganizationName(QStringLiteral("csgify"));
    ProjectLauncherWindow window;
    if (smoke_test
        && (!window.findChild<QPushButton*>(QStringLiteral("newProjectButton"))
            || !window.findChild<QPushButton*>(QStringLiteral("openProjectButton"))))
        return EXIT_FAILURE;
    window.show();
    if (smoke_test) QTimer::singleShot(0, &application, &QCoreApplication::quit);
    return application.exec();
}

} // namespace csgify

#endif
