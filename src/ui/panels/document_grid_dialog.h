/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/document/document_settings.h"

class QWidget;

namespace classiCAD {

bool showDocumentGridDialog(QWidget *parent,
                            const DocumentSettings &initialSettings,
                            DocumentSettings *selectedSettings);

} // namespace classiCAD
