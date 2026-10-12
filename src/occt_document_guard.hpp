#pragma once

#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>

namespace csgify::detail {

// XCAF applications retain every document until Close() is called. Keep that
// ownership bounded for repeated imports and exports in long-running sessions.
class XcafDocumentGuard {
public:
    XcafDocumentGuard(const Handle(XCAFApp_Application)& application,
                      const Handle(TDocStd_Document)& document)
        : application_(application), document_(document) {}

    XcafDocumentGuard(const XcafDocumentGuard&) = delete;
    XcafDocumentGuard& operator=(const XcafDocumentGuard&) = delete;

    ~XcafDocumentGuard() {
        if (application_.IsNull() || document_.IsNull()) return;
        try {
            application_->Close(document_);
        } catch (...) {
            // Destructors cannot report an OCCT close failure usefully. The
            // document handles still release normally during stack unwinding.
        }
    }

private:
    Handle(XCAFApp_Application) application_;
    Handle(TDocStd_Document) document_;
};

} // namespace csgify::detail
