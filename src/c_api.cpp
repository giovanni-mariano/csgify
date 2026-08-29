#include "csgify/c_api.h"

#include "csgify/engine.hpp"
#include "csgify/session.hpp"

#include <cstring>
#include <exception>
#include <string>

struct csgify_session {
    csgify::SessionPtr session;
    std::string last_report_json;
    std::string last_error;

    csgify_session()
        : session(csgify::create_session()) {}
};

namespace {

int fail(csgify_session_t* session, const char* message) {
    if (session) {
        session->last_error = message ? message : "csgify error";
    }
    return CSGIFY_ERROR;
}

void store_report(csgify_session_t* session, const csgify::ConversionReport& report) {
    session->last_report_json = csgify::report_json_string(report);
}

int finish_command(csgify_session_t* session, const csgify::CommandResult& result) {
    store_report(session, result.report);
    session->last_error.clear();
    if (!result.ok || !result.report.ok()) {
        session->last_error = "command completed with error diagnostics";
        return CSGIFY_REPORT_NOT_OK;
    }
    return CSGIFY_OK;
}

csgify::SplineFitKind parse_fit_kind(const char* kind) {
    if (!kind) {
        return csgify::SplineFitKind::none;
    }
    if (std::strcmp(kind, "plane") == 0) {
        return csgify::SplineFitKind::plane;
    }
    if (std::strcmp(kind, "sphere") == 0) {
        return csgify::SplineFitKind::sphere;
    }
    if (std::strcmp(kind, "cylinder") == 0) {
        return csgify::SplineFitKind::cylinder;
    }
    if (std::strcmp(kind, "cone") == 0) {
        return csgify::SplineFitKind::cone;
    }
    return csgify::SplineFitKind::none;
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

csgify_session_t* csgify_session_create(void) {
    try {
        return new csgify_session_t();
    } catch (const std::exception&) {
        return nullptr;
    }
}

void csgify_session_destroy(csgify_session_t* session) {
    delete session;
}

int csgify_load_step(csgify_session_t* session,
                      const char* path,
                      char* document_id_out,
                      size_t document_id_capacity) {
    if (!session || !path) {
        return fail(session, "invalid argument");
    }
    try {
        const csgify::DocumentId id = csgify::load_step(*session->session, path);
        store_report(session, csgify::last_report(*session->session));
        if (!copy_string(id, document_id_out, document_id_capacity)) {
            return fail(session, "document_id buffer too small");
        }
        if (!csgify::last_report(*session->session).ok()) {
            session->last_error = "load_step report is not ok";
            return CSGIFY_REPORT_NOT_OK;
        }
        session->last_error.clear();
        return CSGIFY_OK;
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int csgify_analyze(csgify_session_t* session,
                    const char* document_id) {
    if (!session || !document_id) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(session,
                              csgify::analyze(*session->session, document_id));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

size_t csgify_document_revision(csgify_session_t* session,
                                const char* document_id) {
    if (!session || !document_id) return 0;
    return csgify::document_revision(*session->session, document_id);
}

int csgify_undo_last_mutation(csgify_session_t* session,
                              const char* document_id) {
    if (!session || !document_id) return fail(session, "invalid argument");
    try {
        return finish_command(
            session,
            csgify::undo_last_mutation(*session->session, document_id));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int csgify_remove_solid(csgify_session_t* session,
                         const char* shape_id) {
    if (!session || !shape_id) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(session,
                              csgify::remove_solid(*session->session, shape_id));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int csgify_replace_face_with_fit(csgify_session_t* session,
                                  const char* shape_id,
                                  const char* kind,
                                  double max_deviation) {
    if (!session || !shape_id || !kind) {
        return fail(session, "invalid argument");
    }
    const csgify::SplineFitKind fit_kind = parse_fit_kind(kind);
    if (fit_kind == csgify::SplineFitKind::none) {
        return fail(session, "unknown fit kind");
    }
    try {
        return finish_command(
            session,
            csgify::replace_face_with_fit(*session->session,
                                           shape_id,
                                           fit_kind,
                                           true,
                                           max_deviation));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int csgify_auto_fit_splines(csgify_session_t* session,
                             const char* document_id,
                             double max_deviation) {
    if (!session || !document_id) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(
            session,
            csgify::auto_fit_splines(*session->session,
                                      document_id,
                                      true,
                                      max_deviation));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

int csgify_emit_document(csgify_session_t* session,
                          const char* document_id,
                          const char* output_path) {
    if (!session || !document_id || !output_path) {
        return fail(session, "invalid argument");
    }
    try {
        return finish_command(
            session,
            csgify::emit_document(*session->session, document_id, output_path));
    } catch (const std::exception& e) {
        return fail(session, e.what());
    }
}

const char* csgify_last_report_json(csgify_session_t* session) {
    if (!session) {
        return "";
    }
    return session->last_report_json.c_str();
}

const char* csgify_last_error(csgify_session_t* session) {
    if (!session) {
        return "invalid session";
    }
    return session->last_error.c_str();
}

} // extern "C"
