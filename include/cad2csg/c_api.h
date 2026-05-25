#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cad2csg_session cad2csg_session_t;

enum {
    CAD2CSG_OK = 0,
    CAD2CSG_ERROR = 1,
    CAD2CSG_REPORT_NOT_OK = 2
};

cad2csg_session_t* cad2csg_session_create(void);
void cad2csg_session_destroy(cad2csg_session_t* session);

int cad2csg_load_step(cad2csg_session_t* session,
                      const char* path,
                      char* document_id_out,
                      size_t document_id_capacity);

int cad2csg_analyze(cad2csg_session_t* session,
                    const char* document_id);

int cad2csg_remove_solid(cad2csg_session_t* session,
                         const char* shape_id);

int cad2csg_replace_face_with_fit(cad2csg_session_t* session,
                                  const char* shape_id,
                                  const char* kind,
                                  double max_deviation);

int cad2csg_auto_fit_splines(cad2csg_session_t* session,
                             const char* document_id,
                             double max_deviation);

int cad2csg_emit_document(cad2csg_session_t* session,
                          const char* document_id,
                          const char* output_path);

const char* cad2csg_last_report_json(cad2csg_session_t* session);
const char* cad2csg_last_error(cad2csg_session_t* session);

#ifdef __cplusplus
}
#endif

