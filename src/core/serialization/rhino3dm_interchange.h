#pragma once

#include <QString>

namespace classiCAD {

class Document;

struct Rhino3dmImportReport {
    int importedObjectCount = 0;
    int skippedObjectCount = 0;
    QString warningMessage;
};

// Imports supported Rhino geometry and layers into an existing document.
// Rhino .3dm remains a separate geometry interchange format.
bool importRhino3dmDocument(const QString &path,
                            Document *document,
                            Rhino3dmImportReport *report = nullptr,
                            QString *errorMessage = nullptr);

} // namespace classiCAD
