#pragma once

#include "core/document/shape.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace classiCAD {

QJsonObject pointToJson(const QPointF &point);
bool pointFromJson(const QJsonValue &value, QPointF *point);
QJsonArray pointsToJson(const QVector<QPointF> &points);
bool pointsFromJson(const QJsonValue &value, QVector<QPointF> *points);
QJsonObject nurbsToJson(const Shape::NurbsCurve2D &curve);
bool nurbsFromJson(const QJsonValue &value, Shape::NurbsCurve2D *curve);
QJsonObject nurbsSurfaceToJson(const Shape::NurbsSurface3D &surface);
bool nurbsSurfaceFromJson(const QJsonValue &value,
                          Shape::NurbsSurface3D *surface);
QJsonObject shapeToJson(const Shape &shape);
bool shapeFromJson(const QJsonValue &value, Shape *shape);

} // namespace classiCAD
