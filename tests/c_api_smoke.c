#include "csgify/c_api.h"

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

    csgify_session_t* session = csgify_session_create();
    if (!session) {
        return fail("session create failed");
    }

    char doc[32];
    if (csgify_load_step(session, argv[1], doc, sizeof(doc)) != CSGIFY_OK) {
        fprintf(stderr, "last_error: %s\n", csgify_last_error(session));
        csgify_session_destroy(session);
        return fail("load_step failed");
    }
    if (strcmp(doc, "doc:1") != 0) {
        csgify_session_destroy(session);
        return fail("expected first document to be doc:1");
    }

    if (csgify_analyze(session, doc) != CSGIFY_OK) {
        fprintf(stderr, "last_error: %s\n", csgify_last_error(session));
        csgify_session_destroy(session);
        return fail("analyze failed");
    }

    const char* report = csgify_last_report_json(session);
    if (!report || strstr(report, "\"document\"") == NULL
        || strstr(report, "doc:1/solid:1") == NULL) {
        csgify_session_destroy(session);
        return fail("expected scoped JSON report");
    }

    const int remove_status =
        csgify_remove_solid(session, "doc:1/solid:1");
    const size_t remove_revision = csgify_document_revision(session, doc);
    if (remove_status != CSGIFY_REPORT_NOT_OK || remove_revision != 1) {
        fprintf(stderr, "remove_status=%d revision=%zu error=%s\n",
                remove_status, remove_revision, csgify_last_error(session));
        csgify_session_destroy(session);
        return fail("C API mutation did not create revision 1");
    }
    if (csgify_undo_last_mutation(session, doc) != CSGIFY_OK
        || csgify_document_revision(session, doc) != 2) {
        csgify_session_destroy(session);
        return fail("C API undo did not restore revision 2");
    }

    if (csgify_emit_document(session, doc, argv[2]) != CSGIFY_OK) {
        fprintf(stderr, "last_error: %s\n", csgify_last_error(session));
        csgify_session_destroy(session);
        return fail("emit failed");
    }

    csgify_session_destroy(session);
    return EXIT_SUCCESS;
}
