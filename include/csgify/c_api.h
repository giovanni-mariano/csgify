#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csgify_session csgify_session_t;

enum {
    CSGIFY_OK = 0,
    CSGIFY_ERROR = 1,
    CSGIFY_REPORT_NOT_OK = 2
};

csgify_session_t* csgify_session_create(void);
void csgify_session_destroy(csgify_session_t* session);

int csgify_load_step(csgify_session_t* session,
                      const char* path,
                      char* document_id_out,
                      size_t document_id_capacity);

int csgify_analyze(csgify_session_t* session,
                    const char* document_id);

int csgify_remove_solid(csgify_session_t* session,
                         const char* shape_id);

int csgify_replace_face_with_fit(csgify_session_t* session,
                                  const char* shape_id,
                                  const char* kind,
                                  double max_deviation);

int csgify_auto_fit_splines(csgify_session_t* session,
                             const char* document_id,
                             double max_deviation);

int csgify_emit_document(csgify_session_t* session,
                          const char* document_id,
                          const char* output_path);

const char* csgify_last_report_json(csgify_session_t* session);
const char* csgify_last_error(csgify_session_t* session);

#ifdef __cplusplus
}
#endif

