#pragma once

class QString;

namespace classiCAD {

class Document;

bool saveVignolaDocument(const QString &path,
                         const Document &document,
                         QString *errorMessage = nullptr);

bool loadVignolaDocument(const QString &path,
                         Document *document,
                         QString *errorMessage = nullptr);

} // namespace classiCAD
