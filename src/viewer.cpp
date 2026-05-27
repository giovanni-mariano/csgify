#include "viewer.hpp"
#if CAD2CSG_HAVE_TKOPENGL

#include "session_view.hpp"

#include <AIS_InteractiveContext.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_Plane.hxx>
#include <AIS_Shape.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <Geom_Plane.hxx>
#include <Graphic3d_GraphicDriverFactory.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <OpenGl_GraphicDriverFactory.hxx>
#include <Quantity_Color.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <Xw_Window.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cad2csg {
namespace {

Quantity_Color color_for_status(SolidViewStatus status) {
    switch (status) {
    case SolidViewStatus::exact_csg:
        return Quantity_Color(0.15, 0.75, 0.25, Quantity_TOC_sRGB);
    case SolidViewStatus::needs_decomposition:
        return Quantity_Color(0.90, 0.70, 0.10, Quantity_TOC_sRGB);
    case SolidViewStatus::no_interior_point:
        return Quantity_Color(0.85, 0.15, 0.15, Quantity_TOC_sRGB);
    case SolidViewStatus::piece:
        return Quantity_Color(0.20, 0.55, 0.90, Quantity_TOC_sRGB);
    }
    return Quantity_Color(0.6, 0.6, 0.6, Quantity_TOC_sRGB);
}

const char* label_for_status(SolidViewStatus status) {
    switch (status) {
    case SolidViewStatus::exact_csg:           return "exact-CSG";
    case SolidViewStatus::needs_decomposition: return "needs-decomp";
    case SolidViewStatus::no_interior_point:   return "no-interior-pt";
    case SolidViewStatus::piece:               return "piece";
    }
    return "unknown";
}

const DecompositionCandidate* find_candidate(const DecompositionProposal* proposal,
                                             const std::string& id) {
    if (!proposal || id.empty()) return nullptr;
    for (const DecompositionCandidate& c : proposal->candidates) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

std::string local_id_component(const std::string& id) {
    const std::size_t slash = id.rfind('/');
    if (slash == std::string::npos) {
        return id;
    }
    return id.substr(slash + 1);
}

bool id_matches(const std::string& entry_id, const std::string& requested_id) {
    if (requested_id.empty()) {
        return false;
    }
    return entry_id == requested_id
        || local_id_component(entry_id) == requested_id
        || local_id_component(requested_id) == entry_id;
}

const SolidViewEntry* find_entry_by_id(const std::vector<SolidViewEntry>& entries,
                                       const std::string& id) {
    for (const SolidViewEntry& entry : entries) {
        if (id_matches(entry.shape_id, id)) {
            return &entry;
        }
    }
    return nullptr;
}

void print_selection_details(const SolidViewEntry& entry) {
    std::cerr << "  " << entry.shape_id;
    if (!entry.name.empty()) {
        std::cerr << " (" << entry.name << ")";
    }
    std::cerr << "  " << label_for_status(entry.status);
    if (!entry.source_solid_id.empty()) {
        std::cerr << "  from " << entry.source_solid_id;
    }
    std::cerr << "\n";
}

Handle(AIS_Shape) make_hole_fill_overlay(const HoleFillCandidate& candidate) {
    const double length = candidate.length;
    if (candidate.radius <= 0.0 || length <= 0.0) {
        return {};
    }
    gp_Pnt base(candidate.base.x, candidate.base.y, candidate.base.z);
    gp_Vec height(candidate.height.x, candidate.height.y, candidate.height.z);
    if (height.Magnitude() <= 1.0e-12) {
        return {};
    }
    gp_Dir direction(height);
    gp_Ax2 axis(base, direction);
    BRepPrimAPI_MakeCylinder maker(axis, candidate.radius, length);
    maker.Build();
    if (!maker.IsDone()) {
        return {};
    }
    return new AIS_Shape(maker.Solid());
}

Handle(AIS_InteractiveObject) make_surface_replacement_overlay(
    const SurfaceReplacementCandidate& candidate) {
    if (candidate.kind == "plane") {
        gp_Pnt origin(candidate.origin.x, candidate.origin.y, candidate.origin.z);
        gp_Dir normal(candidate.direction.x,
                      candidate.direction.y,
                      candidate.direction.z);
        Handle(Geom_Plane) geom_plane = new Geom_Plane(gp_Pln(origin, normal));
        Handle(AIS_Plane) ais_plane = new AIS_Plane(geom_plane);
        ais_plane->SetSize(250.0);
        return ais_plane;
    }

    if (candidate.kind == "sphere") {
        if (candidate.radius <= 0.0) return {};
        gp_Pnt center(candidate.origin.x, candidate.origin.y, candidate.origin.z);
        BRepPrimAPI_MakeSphere maker(center, candidate.radius);
        maker.Build();
        if (!maker.IsDone()) return {};
        return new AIS_Shape(maker.Shape());
    }

    if (candidate.kind == "cylinder") {
        if (candidate.radius <= 0.0) return {};
        gp_Pnt origin(candidate.origin.x, candidate.origin.y, candidate.origin.z);
        gp_Dir direction(candidate.direction.x,
                         candidate.direction.y,
                         candidate.direction.z);
        const double length = std::max(candidate.radius * 4.0, 1.0);
        gp_Pnt base(origin.XYZ() - direction.XYZ() * (0.5 * length));
        BRepPrimAPI_MakeCylinder maker(gp_Ax2(base, direction),
                                       candidate.radius,
                                       length);
        maker.Build();
        if (!maker.IsDone()) return {};
        return new AIS_Shape(maker.Solid());
    }

    if (candidate.kind == "cone") {
        if (candidate.half_angle <= 0.0) return {};
        gp_Pnt apex(candidate.origin.x, candidate.origin.y, candidate.origin.z);
        gp_Dir direction(candidate.direction.x,
                         candidate.direction.y,
                         candidate.direction.z);
        const double length = 4.0;
        const double radius = std::tan(candidate.half_angle) * length;
        if (radius <= 0.0) return {};
        BRepPrimAPI_MakeCone maker(gp_Ax2(apex, direction), 0.0, radius, length);
        maker.Build();
        if (!maker.IsDone()) return {};
        return new AIS_Shape(maker.Solid());
    }

    return {};
}

} // namespace

int run_viewer_command(Session& session,
                       const std::string& step_path,
                       const std::string& document_id,
                       const std::string& preselect_id,
                       const DecompositionProposal* proposal,
                       const std::string& candidate_id,
                       const ViewerReviewContext& review,
                       const std::string& commit_out_path) {
    const std::vector<SolidViewEntry> entries = collect_view_entries(session);
    if (entries.empty()) {
        std::cerr << "error viewer_no_shapes: no displayable solids in session.\n";
        return EXIT_FAILURE;
    }

    // Register OpenGL driver if not already done.
    if (Graphic3d_GraphicDriverFactory::DefaultDriverFactory().IsNull()) {
        Graphic3d_GraphicDriverFactory::RegisterFactory(
            new OpenGl_GraphicDriverFactory(), true);
    }

    Handle(Aspect_DisplayConnection) disp_conn = new Aspect_DisplayConnection();

    Handle(Graphic3d_GraphicDriver) gfx_driver =
        Graphic3d_GraphicDriverFactory::DefaultDriverFactory()->CreateDriver(disp_conn);
    if (gfx_driver.IsNull()) {
        std::cerr << "error viewer_no_driver: failed to create OpenGL graphics driver.\n";
        return EXIT_FAILURE;
    }

    Handle(V3d_Viewer) v3d_viewer = new V3d_Viewer(gfx_driver);
    v3d_viewer->SetDefaultLights();
    v3d_viewer->SetLightOn();

    Handle(AIS_InteractiveContext) ctx = new AIS_InteractiveContext(v3d_viewer);
    ctx->SetDisplayMode(AIS_Shaded, false);

    const std::string title = "cad2csg view  —  " + step_path;
    Handle(Xw_Window) xw_win = new Xw_Window(
        disp_conn, title.c_str(), 50, 50, 1100, 750);
    xw_win->Map();

    Handle(V3d_View) view = v3d_viewer->CreateView();
    view->SetWindow(xw_win);
    view->SetBgGradientColors(
        Quantity_Color(0.17, 0.17, 0.22, Quantity_TOC_sRGB),
        Quantity_Color(0.09, 0.09, 0.12, Quantity_TOC_sRGB),
        Aspect_GFM_VER, false);

    // Display all solids, color-coded by status.
    std::map<AIS_InteractiveObject*, std::string> ais_to_id;
    std::vector<std::pair<std::string, Handle(AIS_Shape)>> id_to_ais;
    std::vector<Handle(AIS_Shape)> ais_shapes; // keep-alive storage

    Handle(AIS_Shape) preselect_ais;
    std::string selected_id;
    bool isolated = false;
    bool high_transparency = false;

    for (const SolidViewEntry& entry : entries) {
        if (entry.shape.IsNull()) continue;
        Handle(AIS_Shape) ais = new AIS_Shape(entry.shape);
        ctx->Display(ais, AIS_Shaded, 0, false);
        ctx->SetColor(ais, color_for_status(entry.status), false);
        ctx->SetTransparency(ais, 0.25, false);
        ais_to_id[ais.get()] = entry.shape_id;
        id_to_ais.push_back(std::make_pair(entry.shape_id, ais));
        ais_shapes.push_back(ais);
        if (id_matches(entry.shape_id, preselect_id)) {
            preselect_ais = ais;
            selected_id = entry.shape_id;
        }
    }

    // Optional cut plane overlay from a decomposition candidate.
    const DecompositionCandidate* cand = find_candidate(proposal, candidate_id);
    if (cand) {
        std::vector<PlaneSpec> specs;
        if (!cand->planes.empty()) {
            specs = cand->planes;
        } else if (cand->plane_valid) {
            specs.push_back({cand->plane_origin, cand->plane_normal});
        }
        for (const PlaneSpec& ps : specs) {
            gp_Pnt origin(ps.origin.x, ps.origin.y, ps.origin.z);
            gp_Dir normal(ps.normal.x, ps.normal.y, ps.normal.z);
            Handle(Geom_Plane) geom_plane = new Geom_Plane(gp_Pln(origin, normal));
            Handle(AIS_Plane) ais_plane   = new AIS_Plane(geom_plane);
            ais_plane->SetSize(500.0);
            ctx->Display(ais_plane, 1, -1, false);
            ctx->SetColor(ais_plane,
                Quantity_Color(0.30, 0.50, 1.00, Quantity_TOC_sRGB), false);
            ctx->SetTransparency(ais_plane, 0.55, false);
        }
    }

    Handle(AIS_Shape) hole_fill_overlay;
    if (review.has_hole_fill) {
        hole_fill_overlay = make_hole_fill_overlay(review.hole_fill);
        if (!hole_fill_overlay.IsNull()) {
            ctx->Display(hole_fill_overlay, AIS_Shaded, 0, false);
            ctx->SetColor(hole_fill_overlay,
                          Quantity_Color(0.10, 0.85, 0.95, Quantity_TOC_sRGB),
                          false);
            ctx->SetTransparency(hole_fill_overlay, 0.15, false);
        } else {
            std::cerr << "warning viewer_hole_fill_overlay_failed: "
                      << review.review_id << "\n";
        }
    }

    Handle(AIS_InteractiveObject) surface_overlay;
    if (review.has_surface_replacement) {
        surface_overlay = make_surface_replacement_overlay(
            review.surface_replacement);
        if (!surface_overlay.IsNull()) {
            ctx->Display(surface_overlay, AIS_Shaded, 0, false);
            ctx->SetColor(surface_overlay,
                          Quantity_Color(0.95, 0.35, 0.95, Quantity_TOC_sRGB),
                          false);
            ctx->SetTransparency(surface_overlay, 0.45, false);
        } else {
            std::cerr << "warning viewer_surface_overlay_failed: "
                      << review.review_id << "\n";
        }
    }

    if (!preselect_ais.IsNull()) {
        ctx->SetSelected(preselect_ais, false);
        if (review.kind == "removal") {
            ctx->SetColor(preselect_ais,
                          Quantity_Color(1.00, 0.18, 0.08, Quantity_TOC_sRGB),
                          false);
            ctx->SetTransparency(preselect_ais, 0.05, false);
        } else if (review.kind == "hole_fill") {
            ctx->SetColor(preselect_ais,
                          Quantity_Color(0.90, 0.90, 0.90, Quantity_TOC_sRGB),
                          false);
            ctx->SetTransparency(preselect_ais, 0.65, false);
        } else if (review.kind == "surface_replacement") {
            ctx->SetTransparency(preselect_ais, 0.55, false);
        }
    }

    // --- decisions live in the headless-testable controller ----------------
    // viewer.cpp only renders controller state and forwards key events. The
    // controller owns the alternative state machine and every session mutation
    // (the same calls the CLI and the headless `--keys` path use).
    ViewerController controller(session, document_id, review);
    const bool removal_set_mode = controller.removal_set_mode();

    auto ais_for_id = [&](const std::string& id) -> Handle(AIS_Shape) {
        for (const auto& item : id_to_ais) {
            if (id_matches(item.first, id)) return item.second;
        }
        return {};
    };

    // Recolor removal-set alternatives from controller state: active = bright
    // red, other pending = dim, rejected = restored, accepted = already erased.
    auto recolor_alternatives = [&]() {
        const std::vector<ControllerAlternative>& alts = controller.alternatives();
        const int active = controller.active_index();
        for (int i = 0; i < static_cast<int>(alts.size()); ++i) {
            Handle(AIS_Shape) ais = ais_for_id(alts[i].info.target_id);
            if (ais.IsNull() || alts[i].state == AltState::accepted) continue;
            if (alts[i].state == AltState::rejected) {
                const SolidViewEntry* e =
                    find_entry_by_id(entries, alts[i].info.target_id);
                ctx->SetColor(ais, color_for_status(
                    e ? e->status : SolidViewStatus::needs_decomposition), false);
                ctx->SetTransparency(ais, 0.25, false);
                continue;
            }
            const bool is_active = (i == active);
            ctx->SetColor(ais,
                is_active ? Quantity_Color(1.00, 0.18, 0.08, Quantity_TOC_sRGB)
                          : Quantity_Color(0.80, 0.45, 0.30, Quantity_TOC_sRGB),
                false);
            ctx->SetTransparency(ais, is_active ? 0.05 : 0.55, false);
        }
        view->Redraw();
    };

    // Display the AIS shapes for newly created pieces (looked up by id from the
    // mutated model). No STEP re-import.
    auto sync_added = [&](const std::vector<std::string>& added) {
        if (added.empty()) return;
        const std::vector<SolidViewEntry> now = collect_view_entries(session);
        for (const std::string& id : added) {
            for (const SolidViewEntry& e : now) {
                if (e.shape_id != id || e.shape.IsNull()) continue;
                Handle(AIS_Shape) ais = new AIS_Shape(e.shape);
                ctx->Display(ais, AIS_Shaded, 0, false);
                ctx->SetColor(ais, color_for_status(e.status), false);
                ctx->SetTransparency(ais, high_transparency ? 0.65 : 0.25, false);
                ais_to_id[ais.get()] = e.shape_id;
                id_to_ais.push_back(std::make_pair(e.shape_id, ais));
                ais_shapes.push_back(ais);
                break;
            }
        }
    };

    // Apply one controller action to the display and the output streams.
    auto apply_action = [&](const ViewerAction& a) {
        if (!a.stdout_line.empty()) {
            std::cout << a.stdout_line << "\n";
            std::cout.flush();
        }
        if (!a.message.empty()) std::cerr << "  " << a.message << "\n";
        for (const std::string& id : a.erased) {
            Handle(AIS_Shape) ais = ais_for_id(id);
            if (!ais.IsNull()) ctx->Erase(ais, false);
        }
        sync_added(a.added);
        if (a.alternatives_changed) recolor_alternatives();
        ctx->UpdateCurrentViewer();
        view->Redraw();
    };

    if (removal_set_mode) {
        // Fade non-candidate solids so the candidates stand out.
        for (const auto& item : id_to_ais) {
            bool is_candidate = false;
            for (const ControllerAlternative& a : controller.alternatives()) {
                if (id_matches(item.first, a.info.target_id)) {
                    is_candidate = true;
                    break;
                }
            }
            if (!is_candidate) ctx->SetTransparency(item.second, 0.80, false);
        }
        recolor_alternatives();
    }

    ctx->UpdateCurrentViewer();
    view->FitAll(0.02, true);
    view->Redraw();

    // Legend on stderr so stdout stays clean for selected IDs.
    std::cerr << "\ncad2csg viewer  (" << entries.size() << " solid"
              << (entries.size() == 1 ? "" : "s") << ")  "
              << step_path << "\n";
    std::cerr << "  green  = exact-CSG ready\n";
    std::cerr << "  yellow = needs decomposition\n";
    std::cerr << "  red    = interior_point_not_found\n";
    std::cerr << "  blue   = decomposed piece\n";
    std::cerr << "  orbit=left-drag  pan=middle-drag  zoom=scroll  "
                 "click=select  F=fit\n";
    std::cerr << "  H=hide selected  I=isolate selected  S=show all  "
                 "T=toggle transparency\n";
    std::cerr << "  R=remove selected (logged)  A=accept active (logged)  "
                 "X=reject active  Q/Esc=quit\n";
    if (!commit_out_path.empty())
        std::cerr << "  on quit: emit MCNP deck -> " << commit_out_path << "\n";
    else
        std::cerr << "  (no --out given: decisions are logged, no deck emitted "
                     "on quit)\n";
    if (removal_set_mode) {
        std::cerr << "  removal review: " << controller.alternatives().size()
                  << " candidate(s); Tab/Shift-Tab=cycle active  "
                     "A=accept (remove) active  X=reject active\n";
        std::cerr << "  " << controller.active_summary() << "\n";
    }
    if (!preselect_id.empty())
        std::cerr << "  pre-selected: " << preselect_id << "\n";
    if (cand)
        std::cerr << "  cut plane: " << cand->id
                  << " (" << cand->tool_kind << ", " << cand->confidence << ")\n";
    if (!review.review_id.empty()) {
        std::cerr << "  review: " << review.kind << " " << review.review_id;
        if (!review.target_id.empty()) {
            std::cerr << " target " << review.target_id;
        }
        if (!review.action.empty()) {
            std::cerr << " action " << review.action;
        }
        std::cerr << "\n";
        if (review.has_hole_fill) {
            std::cerr << "  hole-fill preview: radius "
                      << review.hole_fill.radius
                      << " length " << review.hole_fill.length
                      << " volume " << review.hole_fill.plug_volume
                      << "\n";
        }
        if (review.has_surface_replacement) {
            const SurfaceReplacementCandidate& sr = review.surface_replacement;
            std::cerr << "  surface replacement: kind " << sr.kind
                      << " confidence " << sr.confidence
                      << " max_deviation " << sr.max_deviation
                      << " rms_deviation " << sr.rms_deviation
                      << "\n";
        }
    }
    std::cerr << "\n";

    // X11 event loop.
    Display* xdisplay = (Display*)disp_conn->GetDisplayAspect();
    Window   xwindow  = (Window)xw_win->XWindow();

    XSelectInput(xdisplay, xwindow,
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | KeyPressMask | StructureNotifyMask);

    enum class Drag { none, rotate, pan };
    Drag drag_mode = Drag::none;
    int  drag_start_x = 0, drag_start_y = 0;
    int  last_x = 0, last_y = 0;
    bool is_dragging = false;
    constexpr int kDragPx = 4; // pixels before drag is registered

    for (;;) {
        XEvent ev;
        XNextEvent(xdisplay, &ev);

        switch (ev.type) {

        case Expose:
            if (ev.xexpose.count == 0)
                view->Redraw();
            break;

        case ConfigureNotify:
            view->MustBeResized();
            view->Redraw();
            break;

        case ButtonPress: {
            const int x = ev.xbutton.x, y = ev.xbutton.y;
            if (ev.xbutton.button == Button1) {
                drag_mode    = Drag::rotate;
                drag_start_x = x; drag_start_y = y;
                last_x = x; last_y = y;
                is_dragging  = false;
                view->StartRotation(x, y);
            } else if (ev.xbutton.button == Button2) {
                drag_mode   = Drag::pan;
                last_x = x; last_y = y;
                is_dragging = false;
            } else if (ev.xbutton.button == Button4) {
                view->SetZoom(1.10, true);
                view->Redraw();
            } else if (ev.xbutton.button == Button5) {
                view->SetZoom(0.91, true);
                view->Redraw();
            }
            break;
        }

        case ButtonRelease: {
            const int x = ev.xbutton.x, y = ev.xbutton.y;
            if (ev.xbutton.button == Button1) {
                if (!is_dragging) {
                    // Click: select and print solid ID.
                    ctx->MoveTo(x, y, view, false);
                    ctx->SelectDetected(AIS_SelectionScheme_Replace);
                    for (ctx->InitSelected(); ctx->MoreSelected(); ctx->NextSelected()) {
                        Handle(AIS_InteractiveObject) obj = ctx->SelectedInteractive();
                        auto it = ais_to_id.find(obj.get());
                        if (it != ais_to_id.end()) {
                            selected_id = it->second;
                            std::cout << "selected " << selected_id << "\n";
                            std::cout.flush();
                            const SolidViewEntry* entry =
                                find_entry_by_id(entries, selected_id);
                            if (entry) {
                                print_selection_details(*entry);
                            }
                        }
                    }
                    view->Redraw();
                }
                drag_mode = Drag::none;
            } else if (ev.xbutton.button == Button2) {
                drag_mode = Drag::none;
            }
            break;
        }

        case MotionNotify: {
            const int x = ev.xmotion.x, y = ev.xmotion.y;
            if (drag_mode == Drag::rotate) {
                if (!is_dragging) {
                    const int dx = x - drag_start_x, dy = y - drag_start_y;
                    if (dx*dx + dy*dy > kDragPx*kDragPx)
                        is_dragging = true;
                }
                if (is_dragging) {
                    view->Rotation(x, y);
                    view->Redraw();
                }
            } else if (drag_mode == Drag::pan) {
                if (!is_dragging) is_dragging = true;
                view->Pan(x - last_x, -(y - last_y));
                view->Redraw();
            } else {
                ctx->MoveTo(x, y, view, true); // dynamic highlight on hover
            }
            last_x = x; last_y = y;
            break;
        }

        case KeyPress: {
            const KeySym key = XLookupKeysym(&ev.xkey, 0);
            const bool shift = (ev.xkey.state & ShiftMask) != 0;
            if (key == XK_q || key == XK_Q || key == XK_Escape) {
                const bool ok = controller.commit(commit_out_path);
                if (!commit_out_path.empty()) {
                    std::cerr << (ok ? "  emitted deck -> "
                                     : "  emit failed; deck not written "
                                       "(resolve blockers): ")
                              << commit_out_path << "\n";
                }
                return EXIT_SUCCESS;
            } else if (key == XK_f || key == XK_F) {
                view->FitAll(0.02, true);
                view->Redraw();
            } else if (removal_set_mode && key == XK_Tab) {
                apply_action(controller.cycle(!shift));
            } else if (key == XK_a || key == XK_A) {
                apply_action(controller.accept_active());
            } else if (key == XK_x || key == XK_X) {
                apply_action(controller.reject_active());
            } else if (key == XK_r || key == XK_R) {
                apply_action(controller.remove_target(selected_id));
            } else if (key == XK_h || key == XK_H) {
                if (selected_id.empty()) {
                    std::cerr << "  no selected solid; click a solid first.\n";
                } else {
                    for (const auto& item : id_to_ais) {
                        if (id_matches(item.first, selected_id)) {
                            ctx->Erase(item.second, false);
                            std::cerr << "  hidden " << item.first << "\n";
                            break;
                        }
                    }
                    view->Redraw();
                }
            } else if (key == XK_i || key == XK_I) {
                if (selected_id.empty()) {
                    std::cerr << "  no selected solid; click a solid first.\n";
                } else {
                    for (const auto& item : id_to_ais) {
                        if (id_matches(item.first, selected_id)) {
                            ctx->Display(item.second, AIS_Shaded, 0, false);
                            ctx->SetSelected(item.second, false);
                        } else {
                            ctx->Erase(item.second, false);
                        }
                    }
                    isolated = true;
                    std::cerr << "  isolated " << selected_id << "\n";
                    view->Redraw();
                }
            } else if (key == XK_s || key == XK_S) {
                for (const auto& item : id_to_ais) {
                    ctx->Display(item.second, AIS_Shaded, 0, false);
                    ctx->SetTransparency(item.second,
                                         high_transparency ? 0.65 : 0.25,
                                         false);
                }
                isolated = false;
                std::cerr << "  show all\n";
                view->Redraw();
            } else if (key == XK_t || key == XK_T) {
                high_transparency = !high_transparency;
                for (const auto& item : id_to_ais) {
                    ctx->SetTransparency(item.second,
                                         high_transparency ? 0.65 : 0.25,
                                         false);
                }
                std::cerr << "  transparency "
                          << (high_transparency ? "high" : "normal") << "\n";
                if (isolated) {
                    std::cerr << "  isolated view remains active\n";
                }
                view->Redraw();
            }
            break;
        }

        default:
            break;
        }
    }

    return EXIT_SUCCESS;
}

} // namespace cad2csg

#endif // CAD2CSG_HAVE_TKOPENGL
