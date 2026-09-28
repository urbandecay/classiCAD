#pragma once

#include <QString>

namespace classiCAD {

class Document;

struct Rhino3dmImportReport {
    int importedObjectCount = 0;
    int skippedObjectCount = 0;
    QString warningMessage;
};

bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         QString *errorMessage = nullptr);

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         QString *errorMessage = nullptr);

// Imports supported Rhino geometry and layers into an existing document.
// The Vignola document remains the destination's native save format.
bool importRhino3dmDocument(const QString &path,
                            Document *document,
                            Rhino3dmImportReport *report = nullptr,
                            QString *errorMessage = nullptr);

} // namespace classiCAD
