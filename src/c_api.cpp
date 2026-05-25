#include "cad2csg/c_api.h"

#include "cad2csg/engine.hpp"
#include "cad2csg/session.hpp"

#include <cstring>
#include <exception>
#include <string>

struct cad2csg_session {
    cad2csg::SessionPtr session;
    std::string last_report_json;
    std::string last_error;

    cad2csg_session()
        : session(cad2csg::create_session()) {}
};

namespace {

int fail(cad2csg_session_t* session, const char* message) {
    if (session) {
        session->last_error = message ? message : "cad2csg error";
    }
    return CAD2CSG_ERROR;
}

void store_report(cad2csg_session_t* session, const cad2csg::ConversionReport& report) {
    session->last_report_json = cad2csg::report_json_string(report);
}

int finish_command(cad2csg_session_t* session, const cad2csg::CommandResult& result) {
    store_report(session, result.report);
    session->last_error.clear();
    if (!result.ok) {
        session->last_error = "command completed with error diagnostics";
        return CAD2CSG_REPORT_NOT_OK;
    }
    return CAD2CSG_OK;
}

cad2csg::SplineFitKind parse_fit_kind(const char* kind) {
    if (!kind) {
        return cad2csg::SplineFitKind::none;
    }
    if (std::strcmp(kind, "plane") == 0) {
        return cad2csg::SplineFitKind::plane;
    }
    if (std::strcmp(kind, "sphere") == 0) {
        return cad2csg::SplineFitKind::sphere;
    }
    if (std::strcmp(kind, "cylinder") == 0) {
        return cad2csg::SplineFitKind::cylinder;
    }
    if (std::strcmp(kind, "cone") == 0) {
        return cad2csg::SplineFitKind::cone;
    }
    return cad2csg::SplineFitKind::none;
}

bool copy_string(const std::string& value, char* out, size_t capacity) {
    if (!out || capacity == 0 || value.size() + 1 > capacity) {
        return false;
    }
    std::memcpy(out, value.c_str(), value.size() + 1);
    return true;
}

} // namespace

extern "C" {

cad2csg_session_t* cad2csg_session_create(void) {
    try {
        return new cad2csg_session_t();
    } catch (const std::exception&) {
        return nullptr;
    }
}

void cad2csg_session_destroy(cad2csg_session_t* session) {
    delete session;
}

int cad2csg_load_step(cad2csg_session_t* session,
                      const char* path,
                      char* document_id_out,
                      size_t document_id_capacity) {
    if (!session || !path) {
        return fail(session, "invalid argument");
    }
    try {
        const cad2csg::DocumentId id = cad2csg::load_step(*session->session, path);
        store_report(session, cad2csg::last_report(*session->session));
        if (!copy_string(id, document_id_out, document_id_capacity)) {
            return fail(session, "document_id buffer too small");
        }
        session->last_error.clear();
        return CAD2CSG_OK;
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int cad2csg_analyze(cad2csg_session_t* session,
                    const char* document_id) {
    if (!session || !document_id) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(session,
                              cad2csg::analyze(*session->session, document_id));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int cad2csg_remove_solid(cad2csg_session_t* session,
                         const char* shape_id) {
    if (!session || !shape_id) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(session,
                              cad2csg::remove_solid(*session->session, shape_id));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int cad2csg_replace_face_with_fit(cad2csg_session_t* session,
                                  const char* shape_id,
                                  const char* kind,
                                  double max_deviation) {
    if (!session || !shape_id || !kind) {
        return fail(session, "invalid argument");
    }
    const cad2csg::SplineFitKind fit_kind = parse_fit_kind(kind);
    if (fit_kind == cad2csg::SplineFitKind::none) {
        return fail(session, "unknown fit kind");
    }
    try {
        return finish_command(
            session,
            cad2csg::replace_face_with_fit(*session->session,
                                           shape_id,
                                           fit_kind,
                                           true,
                                           max_deviation));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int cad2csg_auto_fit_splines(cad2csg_session_t* session,
                             const char* document_id,
                             double max_deviation) {
    if (!session || !document_id) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(
            session,
            cad2csg::auto_fit_splines(*session->session,
                                      document_id,
                                      true,
                                      max_deviation));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int cad2csg_emit_document(cad2csg_session_t* session,
                          const char* document_id,
                          const char* output_path) {
    if (!session || !document_id || !output_path) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(
            session,
            cad2csg::emit_document(*session->session, document_id, output_path));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

const char* cad2csg_last_report_json(cad2csg_session_t* session) {
    if (!session) {
        return "";
    }
    return session->last_report_json.c_str();
}

const char* cad2csg_last_error(cad2csg_session_t* session) {
    if (!session) {
        return "invalid session";
    }
    return session->last_error.c_str();
}

} // extern "C"

