#include "cad2csg/c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* message) {
    fprintf(stderr, "c_api_smoke: %s\n", message);
    return EXIT_FAILURE;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return fail("usage: c_api_smoke STEP OUT");
    }

    cad2csg_session_t* session = cad2csg_session_create();
    if (!session) {
        return fail("session create failed");
    }

    char doc[32];
    if (cad2csg_load_step(session, argv[1], doc, sizeof(doc)) != CAD2CSG_OK) {
        fprintf(stderr, "last_error: %s\n", cad2csg_last_error(session));
        cad2csg_session_destroy(session);
        return fail("load_step failed");
    }
    if (strcmp(doc, "doc:1") != 0) {
        cad2csg_session_destroy(session);
        return fail("expected first document to be doc:1");
    }

    if (cad2csg_analyze(session, doc) != CAD2CSG_OK) {
        fprintf(stderr, "last_error: %s\n", cad2csg_last_error(session));
        cad2csg_session_destroy(session);
        return fail("analyze failed");
    }

    const char* report = cad2csg_last_report_json(session);
    if (!report || strstr(report, "\"document\"") == NULL
        || strstr(report, "doc:1/solid:1") == NULL) {
        cad2csg_session_destroy(session);
        return fail("expected scoped JSON report");
    }

    if (cad2csg_emit_document(session, doc, argv[2]) != CAD2CSG_OK) {
        fprintf(stderr, "last_error: %s\n", cad2csg_last_error(session));
        cad2csg_session_destroy(session);
        return fail("emit failed");
    }

    cad2csg_session_destroy(session);
    return EXIT_SUCCESS;
}

