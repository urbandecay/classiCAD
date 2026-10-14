/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_volume.h"

#include "core/geometry/scalar_volume.h"
#include "core/geometry/work_plane.h"

#include <QMatrix4x4>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace classiCAD {
namespace {

constexpr std::uint64_t kMaxDenseVoxels = 16ull * 1024ull * 1024ull;
constexpr qsizetype kMaxSamples = 1'000'000;

struct Vec3 { double x, y, z; };
struct Vertex { QPointF screen; Vec3 grid; double cameraDepth; double projectionDepth; };
struct SliceVertex { Vec3 world; Vec3 grid; double cameraDepth; };

Vec3 mapPoint(const ScalarVolumeMatrix &m, Vec3 p)
{
    return {m[0]*p.x + m[1]*p.y + m[2]*p.z + m[3],
            m[4]*p.x + m[5]*p.y + m[6]*p.z + m[7],
            m[8]*p.x + m[9]*p.y + m[10]*p.z + m[11]};
}

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float denseFetch(const QVector<float> &data, int nx, int ny, int nz,
                 int x, int y, int z)
{
    x = std::clamp(x, 0, nx - 1); y = std::clamp(y, 0, ny - 1);
    z = std::clamp(z, 0, nz - 1);
    return data[(qsizetype(z) * ny + y) * nx + x];
}

float linearAxis(const QVector<float> &data, int nx, int ny, int nz,
                 double cx, double cy, double cz)
{
    const double x = cx - 0.5, y = cy - 0.5, z = cz - 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y)), z0 = int(std::floor(z));
    const double fx = x - x0, fy = y - y0, fz = z - z0;
    double sum = 0.0;
    for (int iz = 0; iz < 2; ++iz) for (int iy = 0; iy < 2; ++iy) for (int ix = 0; ix < 2; ++ix) {
        const double w = (ix ? fx : 1.0-fx) * (iy ? fy : 1.0-fy) * (iz ? fz : 1.0-fz);
        sum += denseFetch(data,nx,ny,nz,x0+ix,y0+iy,z0+iz) * w;
    }
    return float(sum);
}

// Adapts the sampling modes used by Blender v5.2.2:
// https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh
// CLOSEST uses floor(texCoord * size), LINEAR uses clamp-to-edge texture
// filtering, and CUBIC uses the shader's four-tap B-spline weights.
float sampleDensity(const QVector<float> &data, int nx, int ny, int nz,
                    const QString &method, Vec3 grid, const ScalarVolume3D &v)
{
    const double cx = grid.x - v.activeBounds[0] + 0.5;
    const double cy = grid.y - v.activeBounds[1] + 0.5;
    const double cz = grid.z - v.activeBounds[2] + 0.5;
    if (method == QStringLiteral("CLOSEST"))
        return denseFetch(data,nx,ny,nz,int(std::floor(cx)),int(std::floor(cy)),int(std::floor(cz)));
    if (method != QStringLiteral("CUBIC")) return linearAxis(data,nx,ny,nz,cx,cy,cz);

    const auto weights = [](double c, std::array<double,2> *positions,
                            std::array<double,2> *w) {
        const double tc = std::floor(c - 0.5) + 0.5, f = c - tc;
        const double f2=f*f, f3=f2*f;
        const double w3=f3/6.0, w0=-w3+f2*0.5-f*0.5+1.0/6.0;
        const double w1=f3*0.5-f2+2.0/3.0, w2=1.0-w0-w1-w3;
        const double s0=w0+w1, s1=w2+w3;
        positions->at(0)=tc-1.0+w1/std::max(s0,1.0e-20);
        positions->at(1)=tc+1.0+w3/std::max(s1,1.0e-20);
        w->at(0)=s0; w->at(1)=s1;
    };
    std::array<double,2> px{},py{},pz{},wx{},wy{},wz{};
    weights(cx,&px,&wx); weights(cy,&py,&wy); weights(cz,&pz,&wz);
    double result=0.0;
    for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x)
        result += linearAxis(data,nx,ny,nz,px[x],py[y],pz[z])*wx[x]*wy[y]*wz[z];
    return float(result);
}

bool rasterTriangle(const std::array<Vertex,3> &v, qreal dpr,
                    int width, int height, const QVector<float> &dense,
                    int nx,int ny,int nz,const ScalarVolume3D &volume,
                    QVector<RasterWorkbenchVolumeSliceSample> *out,
                    std::unordered_set<std::uint64_t> *seen,
                    float stepWorld)
{
    std::array<QPointF,3> p;
    for(int i=0;i<3;++i) p[i]=v[i].screen*dpr;
    const double area=(p[1].x()-p[0].x())*(p[2].y()-p[0].y())-
                      (p[1].y()-p[0].y())*(p[2].x()-p[0].x());
    if(!std::isfinite(area)||std::abs(area)<1e-12) return true;
    const int minX=std::max(0,int(std::floor(std::min({p[0].x(),p[1].x(),p[2].x()}))));
    const int maxX=std::min(width-1,int(std::ceil(std::max({p[0].x(),p[1].x(),p[2].x()}))));
    const int minY=std::max(0,int(std::floor(std::min({p[0].y(),p[1].y(),p[2].y()}))));
    const int maxY=std::min(height-1,int(std::ceil(std::max({p[0].y(),p[1].y(),p[2].y()}))));
    for(int y=minY;y<=maxY;++y) for(int x=minX;x<=maxX;++x) {
        const double sx=x+0.5, sy=y+0.5;
        const double b0=((p[1].x()-sx)*(p[2].y()-sy)-(p[1].y()-sy)*(p[2].x()-sx))/area;
        const double b1=((p[2].x()-sx)*(p[0].y()-sy)-(p[2].y()-sy)*(p[0].x()-sx))/area;
        const double b2=1.0-b0-b1;
        if(b0 < -1e-9 || b1 < -1e-9 || b2 < -1e-9) continue;
        const double reciprocal=b0/v[0].projectionDepth+b1/v[1].projectionDepth+b2/v[2].projectionDepth;
        if(!(reciprocal>1e-15) || !std::isfinite(reciprocal)) continue;
        const double w0=(b0/v[0].projectionDepth)/reciprocal, w1=(b1/v[1].projectionDepth)/reciprocal,
                     w2=(b2/v[2].projectionDepth)/reciprocal;
        Vec3 g{w0*v[0].grid.x+w1*v[1].grid.x+w2*v[2].grid.x,
               w0*v[0].grid.y+w1*v[1].grid.y+w2*v[2].grid.y,
               w0*v[0].grid.z+w1*v[1].grid.z+w2*v[2].grid.z};
        const std::uint64_t key=std::uint64_t(y)*std::uint64_t(width)+std::uint64_t(x);
        if(!seen->insert(key).second) continue;
        if(out->size()>=kMaxSamples) return false;
        const double depth=w0*v[0].cameraDepth+w1*v[1].cameraDepth+w2*v[2].cameraDepth;
        RasterWorkbenchVolumeSliceSample sample;
        sample.x=x; sample.y=y; sample.cameraDepth=float(depth);
        sample.density=sampleDensity(dense,nx,ny,nz,volume.interpolationMethod,g,volume);
        sample.sliceStepWorld=stepWorld;
        out->append(sample);
    }
    return true;
}

} // namespace

bool rasterWorkbenchVolumeSliceSamples(
    const ViewportRenderObject &object, const ViewportTransform &camera,
    const QSize &viewportSize, qreal devicePixelRatio,
    QVector<RasterWorkbenchVolumeSliceSample> *samples)
{
    if(samples) samples->clear();
    if(!samples || object.shape.geometryType!=GeometryType::ScalarVolume ||
       viewportSize.width()<=0 || viewportSize.height()<=0 ||
       !std::isfinite(devicePixelRatio) || devicePixelRatio<=0.0) return false;
    const ScalarVolume3D &v=object.shape.scalarVolume;
    if(!v.useSlice || !validateScalarVolume(v)) return false;
    const int nx=v.activeBounds[3]-v.activeBounds[0]+1,
              ny=v.activeBounds[4]-v.activeBounds[1]+1,
              nz=v.activeBounds[5]-v.activeBounds[2]+1;
    const std::uint64_t count=std::uint64_t(nx)*std::uint64_t(ny)*std::uint64_t(nz);
    if(nx<=0||ny<=0||nz<=0||count>kMaxDenseVoxels) return false;
    QVector<float> dense(qsizetype(count),v.background);
    for(const ScalarVolumeActiveRecord &r:v.activeRecords)
        for(int z=0;z<r.extent;++z) for(int y=0;y<r.extent;++y) for(int x=0;x<r.extent;++x)
            dense[(qsizetype(r.z+z-v.activeBounds[2])*ny+r.y+y-v.activeBounds[1])*nx+r.x+x-v.activeBounds[0]]=r.value;

    int axis=-1;
    const QString requested=v.sliceAxis.toUpper();
    if(requested.endsWith(QLatin1Char('X'))) axis=0;
    else if(requested.endsWith(QLatin1Char('Y'))) axis=1;
    else if(requested.endsWith(QLatin1Char('Z'))) axis=2;
    const Point3D view=camera.viewDirection();
    if(axis<0) axis=std::abs(view.x)>=std::abs(view.y)&&std::abs(view.x)>=std::abs(view.z)?0:
                     std::abs(view.y)>=std::abs(view.z)?1:2;
    const double mins[3]={double(v.activeBounds[0])-0.5,double(v.activeBounds[1])-0.5,double(v.activeBounds[2])-0.5};
    const double maxs[3]={double(v.activeBounds[3])+0.5,double(v.activeBounds[4])+0.5,double(v.activeBounds[5])+0.5};
    const int sizes[3]={nx,ny,nz};
    double plane=mins[axis]+std::clamp(double(v.sliceDepth),0.0,1.0)*sizes[axis];
    std::array<Vec3,4> grid{};
    int a=(axis+1)%3,b=(axis+2)%3;
    for(int i=0;i<4;++i) {
        double c[3]={mins[0],mins[1],mins[2]};
        c[axis]=plane; c[a]=(i&1)?maxs[a]:mins[a]; c[b]=(i&2)?maxs[b]:mins[b];
        grid[i]={c[0],c[1],c[2]};
    }
    // Blender v5.2.2 uses the selected object dimension for slice opacity.
    // Object dimensions apply object scale to the object-space bounds, but do
    // not rotate those dimensions into world-axis-aligned extents.
    const ScalarVolumeMatrix &m=v.gridToObject;
    const ScalarVolumeMatrix &n=v.objectToWorld;
    auto worldFor=[&](Vec3 g){Vec3 q=mapPoint(m,g); q=mapPoint(n,q); q.x+=object.placementTranslation.x; q.y+=object.placementTranslation.y; q.z+=object.placementTranslation.z; return q;};
    Vec3 objectLow{std::numeric_limits<double>::max(),
                   std::numeric_limits<double>::max(),
                   std::numeric_limits<double>::max()};
    Vec3 objectHigh{-objectLow.x, -objectLow.y, -objectLow.z};
    for (int i = 0; i < 8; ++i) {
        const Vec3 gridPoint{(i & 1) ? maxs[0] : mins[0],
                             (i & 2) ? maxs[1] : mins[1],
                             (i & 4) ? maxs[2] : mins[2]};
        const Vec3 objectPoint = mapPoint(m, gridPoint);
        if (!finite(objectPoint) || !finite(worldFor(gridPoint))) return false;
        objectLow.x = std::min(objectLow.x, objectPoint.x);
        objectLow.y = std::min(objectLow.y, objectPoint.y);
        objectLow.z = std::min(objectLow.z, objectPoint.z);
        objectHigh.x = std::max(objectHigh.x, objectPoint.x);
        objectHigh.y = std::max(objectHigh.y, objectPoint.y);
        objectHigh.z = std::max(objectHigh.z, objectPoint.z);
    }
    const double objectExtent[3] = {objectHigh.x - objectLow.x,
                                    objectHigh.y - objectLow.y,
                                    objectHigh.z - objectLow.z};
    const double objectScale[3] = {std::hypot(n[0], n[4], n[8]),
                                   std::hypot(n[1], n[5], n[9]),
                                   std::hypot(n[2], n[6], n[10])};
    const float step = float(std::max(
        1e-16, objectExtent[axis] * objectScale[axis] * 0.05));
    std::array<SliceVertex,4> vertices;
    const Point3D eye=camera.cameraPosition(viewportSize), forward=camera.viewDirection();
    for(int i=0;i<4;++i){Vec3 w=worldFor(grid[i]);if(!finite(w))return false;double d=-((w.x-eye.x)*forward.x+(w.y-eye.y)*forward.y+(w.z-eye.z)*forward.z);if(!std::isfinite(d))return false;vertices[i]={w,grid[i],d};}
    const double widthPixels = viewportSize.width() * devicePixelRatio;
    const double heightPixels = viewportSize.height() * devicePixelRatio;
    constexpr qint64 kMaximumRasterPixels = 64ll * 1024ll * 1024ll;
    if (!std::isfinite(widthPixels) || !std::isfinite(heightPixels) ||
        widthPixels <= 0.0 || heightPixels <= 0.0 ||
        widthPixels > kMaximumRasterPixels ||
        heightPixels > kMaximumRasterPixels) return false;
    const qint64 width = qRound64(widthPixels);
    const qint64 height = qRound64(heightPixels);
    if (width <= 0 || height <= 0 ||
        width > kMaximumRasterPixels / height) return false;
    std::unordered_set<std::uint64_t> seen;
    const double nearDepth=std::max(1.0e-9,camera.cameraPreferences().clipStart);
    const auto rasterClippedTriangle=[&](const std::array<SliceVertex,3> &triangle) {
        std::array<SliceVertex,4> clipped{};
        int clippedCount=0;
        SliceVertex previous=triangle.back();
        bool previousInside=previous.cameraDepth>=nearDepth;
        for(const SliceVertex &current:triangle){
            const bool currentInside=current.cameraDepth>=nearDepth;
            if(currentInside!=previousInside){
                const double denominator=current.cameraDepth-previous.cameraDepth;
                if(std::abs(denominator)<=1.0e-15)return true;
                const double t=std::clamp((nearDepth-previous.cameraDepth)/denominator,0.0,1.0);
                clipped[clippedCount++]={{previous.world.x+(current.world.x-previous.world.x)*t,
                                           previous.world.y+(current.world.y-previous.world.y)*t,
                                           previous.world.z+(current.world.z-previous.world.z)*t},
                                          {previous.grid.x+(current.grid.x-previous.grid.x)*t,
                                           previous.grid.y+(current.grid.y-previous.grid.y)*t,
                                           previous.grid.z+(current.grid.z-previous.grid.z)*t},nearDepth};
            }
            if(currentInside)clipped[clippedCount++]=current;
            previous=current;
            previousInside=currentInside;
        }
        if(clippedCount<3)return true;
        std::array<Vertex,4> projectedVertices{};
        for(int i=0;i<clippedCount;++i){
            const SliceVertex &source=clipped[i];
            QPointF screen;
            if(!camera.worldPointToScreenUnclipped(
                   {source.world.x,source.world.y,source.world.z},viewportSize,&screen))return false;
            const double projectionDepth=camera.isPerspectiveEnabled()?source.cameraDepth:1.0;
            if(projectionDepth<=1.0e-15)return false;
            projectedVertices[i]={screen,source.grid,source.cameraDepth,projectionDepth};
        }
        for(int i=1;i+1<clippedCount;++i){
            if(!rasterTriangle({projectedVertices[0],projectedVertices[i],projectedVertices[i+1]},
                               devicePixelRatio,int(width),int(height),dense,nx,ny,nz,v,samples,&seen,step))return false;
        }
        return true;
    };
    const std::array<SliceVertex,3> t0{vertices[0],vertices[1],vertices[3]}, t1{vertices[0],vertices[3],vertices[2]};
    if(!rasterClippedTriangle(t0)||!rasterClippedTriangle(t1)){samples->clear();return false;}
    return !samples->isEmpty();
}

bool rasterWorkbenchVolumeRaySamples(
    const ViewportRenderObject &object, const ViewportTransform &camera,
    const QSize &viewportSize, qreal devicePixelRatio,
    const QVector<float> &sceneDepth, const QVector<bool> &inFrontCoverage,
    std::size_t maximumIntegrationSamples,
    std::size_t *integratedSampleCount,
    QVector<RasterWorkbenchVolumeRaySample> *samples)
{
    if (integratedSampleCount) *integratedSampleCount = 0;
    if (samples) samples->clear();
    if (!samples || !integratedSampleCount || maximumIntegrationSamples == 0 ||
        object.shape.geometryType != GeometryType::ScalarVolume ||
        object.shape.scalarVolume.useSlice || viewportSize.width() <= 0 ||
        viewportSize.height() <= 0 || !std::isfinite(devicePixelRatio) ||
        devicePixelRatio <= 0.0) return false;

    const ScalarVolume3D &volume = object.shape.scalarVolume;
    if (!validateScalarVolume(volume)) return false;
    const int nx = volume.activeBounds[3] - volume.activeBounds[0] + 1;
    const int ny = volume.activeBounds[4] - volume.activeBounds[1] + 1;
    const int nz = volume.activeBounds[5] - volume.activeBounds[2] + 1;
    const std::uint64_t voxelCount = std::uint64_t(nx) *
                                     std::uint64_t(ny) *
                                     std::uint64_t(nz);
    constexpr std::uint64_t kMaximumDenseVoxels = 16ull * 1024ull * 1024ull;
    constexpr std::size_t kMaximumRayPixels = 1'000'000;
    constexpr std::size_t kMaximumIntegratedSamples = 4'000'000;
    if (nx <= 0 || ny <= 0 || nz <= 0 ||
        voxelCount > kMaximumDenseVoxels) return false;
    QVector<float> dense(qsizetype(voxelCount), volume.background);
    for (const ScalarVolumeActiveRecord &record : volume.activeRecords) {
        for (int z = 0; z < record.extent; ++z) {
            for (int y = 0; y < record.extent; ++y) {
                for (int x = 0; x < record.extent; ++x) {
                    dense[(qsizetype(record.z + z - volume.activeBounds[2]) * ny +
                           record.y + y - volume.activeBounds[1]) * nx +
                          record.x + x - volume.activeBounds[0]] = record.value;
                }
            }
        }
    }

    const double widthPixels = viewportSize.width() * devicePixelRatio;
    const double heightPixels = viewportSize.height() * devicePixelRatio;
    constexpr qint64 kMaximumRasterPixels = 1'000'000;
    if (!std::isfinite(widthPixels) || !std::isfinite(heightPixels) ||
        widthPixels <= 0.0 || heightPixels <= 0.0 ||
        widthPixels > kMaximumRasterPixels ||
        heightPixels > kMaximumRasterPixels) return false;
    const int width = qRound64(widthPixels);
    const int height = qRound64(heightPixels);
    const qsizetype pixelCount = qsizetype(width) * height;
    if (width <= 0 || height <= 0 || pixelCount <= 0 ||
        pixelCount > kMaximumRasterPixels || sceneDepth.size() != pixelCount ||
        inFrontCoverage.size() != pixelCount) return false;

    QMatrix4x4 gridToObject;
    QMatrix4x4 objectToWorld;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const std::size_t index = std::size_t(row * 4 + column);
            gridToObject(row, column) = float(volume.gridToObject[index]);
            objectToWorld(row, column) = float(volume.objectToWorld[index]);
        }
    }
    QMatrix4x4 placement;
    placement.translate(float(object.placementTranslation.x),
                        float(object.placementTranslation.y),
                        float(object.placementTranslation.z));
    const QMatrix4x4 gridToWorld = placement * objectToWorld * gridToObject;
    bool invertible = false;
    const QMatrix4x4 worldToGrid = gridToWorld.inverted(&invertible);
    if (!invertible) return false;

    const double gridMin[3] = {
        double(volume.activeBounds[0]) - 0.5,
        double(volume.activeBounds[1]) - 0.5,
        double(volume.activeBounds[2]) - 0.5};
    const double gridMax[3] = {
        double(volume.activeBounds[3]) + 0.5,
        double(volume.activeBounds[4]) + 0.5,
        double(volume.activeBounds[5]) + 0.5};
    QPointF projected[8];
    qreal minScreenX = std::numeric_limits<qreal>::max();
    qreal maxScreenX = std::numeric_limits<qreal>::lowest();
    qreal minScreenY = std::numeric_limits<qreal>::max();
    qreal maxScreenY = std::numeric_limits<qreal>::lowest();
    bool allCornersProjected = true;
    for (int corner = 0; corner < 8; ++corner) {
        const QVector3D gridPoint(
            float((corner & 1) ? gridMax[0] : gridMin[0]),
            float((corner & 2) ? gridMax[1] : gridMin[1]),
            float((corner & 4) ? gridMax[2] : gridMin[2]));
        const QVector3D worldPoint = gridToWorld.map(gridPoint);
        if (!camera.worldPointToScreenUnclipped(
                {worldPoint.x(), worldPoint.y(), worldPoint.z()},
                viewportSize, &projected[corner])) {
            allCornersProjected = false;
            break;
        }
        minScreenX = std::min(minScreenX, projected[corner].x());
        maxScreenX = std::max(maxScreenX, projected[corner].x());
        minScreenY = std::min(minScreenY, projected[corner].y());
        maxScreenY = std::max(maxScreenY, projected[corner].y());
    }
    // A perspective box crossing the camera or near plane can have corners
    // behind the eye, where projection is undefined. Keep the search
    // conservative in that case; the per-pixel ray/box test and near clip below
    // determine actual visibility, while the existing pixel and integration
    // caps bound the fallback cost.
    const int minX = allCornersProjected
        ? std::clamp(qFloor(minScreenX * devicePixelRatio), 0, width - 1)
        : 0;
    const int maxX = allCornersProjected
        ? std::clamp(qCeil(maxScreenX * devicePixelRatio), 0, width - 1)
        : width - 1;
    const int minY = allCornersProjected
        ? std::clamp(qFloor(minScreenY * devicePixelRatio), 0, height - 1)
        : 0;
    const int maxY = allCornersProjected
        ? std::clamp(qCeil(maxScreenY * devicePixelRatio), 0, height - 1)
        : height - 1;
    if (maxX < minX || maxY < minY) return false;
    const std::size_t candidatePixels = std::size_t(maxX - minX + 1) *
                                        std::size_t(maxY - minY + 1);
    if (candidatePixels > kMaximumRayPixels) return false;

    const Point3D target = camera.viewTarget();
    const Point3D viewDirection = camera.viewDirection();
    const Point3D cameraPosition = camera.cameraPosition(viewportSize);
    const WorkPlaneFrame viewPlane = makeWorkPlaneFrameFromNormal(
        target, viewDirection);
    if (!isValidWorkPlaneFrame(viewPlane)) return false;

    const auto rayBoxInterval = [&](QVector3D origin, QVector3D direction,
                                    double *nearT, double *farT) {
        *nearT = 0.0;
        *farT = std::numeric_limits<double>::infinity();
        const double origins[3] = {origin.x(), origin.y(), origin.z()};
        const double directions[3] = {
            direction.x(), direction.y(), direction.z()};
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(directions[axis]) <= 1.0e-12) {
                if (origins[axis] < gridMin[axis] ||
                    origins[axis] > gridMax[axis]) return false;
                continue;
            }
            const double a = (gridMin[axis] - origins[axis]) /
                             directions[axis];
            const double b = (gridMax[axis] - origins[axis]) /
                             directions[axis];
            *nearT = std::max(*nearT, std::min(a, b));
            *farT = std::min(*farT, std::max(a, b));
        }
        return *farT > *nearT;
    };

    const QVector3D worldSize(
        gridToWorld.mapVector(QVector3D(float(nx), 0.0f, 0.0f)).length(),
        gridToWorld.mapVector(QVector3D(0.0f, float(ny), 0.0f)).length(),
        gridToWorld.mapVector(QVector3D(0.0f, 0.0f, float(nz))).length());
    const QVector3D sampleSpacing(worldSize.x() / (float(nx) * 5.0f),
                                  worldSize.y() / (float(ny) * 5.0f),
                                  worldSize.z() / (float(nz) * 5.0f));
    const float stepLength = std::max(1.0e-16f, sampleSpacing.length());
    const int maxSteps = std::min(2560, std::max({nx, ny, nz}) * 5);
    const float densityScale = std::max(0.0f, volume.densityScale);
    const auto bayerNoise = [](int x, int y) {
        static constexpr int matrix[16] = {
            0, 8, 2, 10, 12, 4, 14, 6,
            3, 11, 1, 9, 15, 7, 13, 5};
        return (float(matrix[(x & 3) * 4 + (y & 3)]) + 0.5f) / 16.0f;
    };
    std::size_t integratedSamples = 0;
    samples->reserve(qsizetype(std::min(candidatePixels, kMaximumRayPixels)));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const qsizetype pixelIndex = qsizetype(y) * width + x;
            if (inFrontCoverage[pixelIndex]) continue;
            QPointF viewPlanePosition;
            const QPointF logicalPixel((x + 0.5) / devicePixelRatio,
                                      (y + 0.5) / devicePixelRatio);
            if (!camera.screenToWorkPlaneUnclipped(
                    logicalPixel, viewportSize, viewPlane, &viewPlanePosition))
                continue;
            const Point3D planePoint = workPlaneFramePointToWorld(
                viewPlanePosition, viewPlane);
            QVector3D rayOrigin;
            QVector3D rayDirection;
            if (camera.isPerspectiveEnabled()) {
                rayOrigin = QVector3D(float(cameraPosition.x),
                                      float(cameraPosition.y),
                                      float(cameraPosition.z));
                rayDirection = QVector3D(float(planePoint.x - cameraPosition.x),
                                         float(planePoint.y - cameraPosition.y),
                                         float(planePoint.z - cameraPosition.z));
                if (rayDirection.lengthSquared() <= 1.0e-20f) continue;
                rayDirection.normalize();
            } else {
                rayOrigin = QVector3D(float(planePoint.x), float(planePoint.y),
                                      float(planePoint.z));
                rayDirection = QVector3D(float(-viewDirection.x),
                                         float(-viewDirection.y),
                                         float(-viewDirection.z));
            }
            const float viewCosine = std::max(
                std::abs(QVector3D::dotProduct(
                    rayDirection,
                    QVector3D(float(viewDirection.x), float(viewDirection.y),
                              float(viewDirection.z)))),
                1.0e-6f);
            const QVector3D gridOrigin = worldToGrid.map(rayOrigin);
            const QVector3D gridDirection = worldToGrid.mapVector(rayDirection);
            double nearT = 0.0;
            double farT = 0.0;
            if (!rayBoxInterval(gridOrigin, gridDirection, &nearT, &farT))
                continue;
            const float depthLimit = std::isfinite(sceneDepth[pixelIndex])
                ? sceneDepth[pixelIndex]
                : float(camera.cameraPreferences().clipEnd);
            const float depthPerWorldT = std::max(
                -QVector3D::dotProduct(
                    rayDirection,
                    QVector3D(float(viewDirection.x), float(viewDirection.y),
                              float(viewDirection.z))),
                1.0e-6f);
            const double clipT = depthLimit / depthPerWorldT;
            const double nearClipT = camera.cameraPreferences().clipStart /
                                     depthPerWorldT;
            nearT = std::max(nearT, nearClipT);
            farT = std::min(farT, clipT);
            if (farT <= nearT) continue;

            const double worldChordLength = farT - nearT;
            const float rayStepWorld = stepLength / viewCosine;
            if (rayStepWorld <= 0.0f || !std::isfinite(rayStepWorld)) continue;
            const float noise = bayerNoise(x, y);
            float transmittance = 1.0f;
            QVector3D radiance;
            bool exhaustedWork = false;
            for (int step = 0; step < maxSteps; ++step) {
                const double rayOffset = (step + noise) * rayStepWorld;
                if (rayOffset >= worldChordLength || transmittance <= 0.01f)
                    break;
                if (integratedSamples >= std::min(
                        kMaximumIntegratedSamples,
                        maximumIntegrationSamples)) {
                    exhaustedWork = true;
                    break;
                }
                ++integratedSamples;
                *integratedSampleCount = integratedSamples;
                const double rayT = nearT + rayOffset;
                const QVector3D gridPoint = gridOrigin +
                    gridDirection * float(rayT);
                const float density = sampleDensity(
                    dense, nx, ny, nz, volume.interpolationMethod,
                    {gridPoint.x(), gridPoint.y(), gridPoint.z()}, volume) *
                    densityScale;
                const float extinction = std::max(density, 1.0e-4f);
                const float stepTransmittance = std::exp(
                    -extinction * rayStepWorld);
                const float stepOpacity = 1.0f - stepTransmittance;
                const float scatteringScale = density / extinction;
                const float scattering = transmittance * stepOpacity *
                                         scatteringScale * 0.25f;
                radiance += QVector3D(scattering, scattering, scattering);
                transmittance *= stepTransmittance;
            }
            if (exhaustedWork) {
                samples->clear();
                return false;
            }
            const float alpha = 1.0f - transmittance;
            if (alpha <= 0.0f) continue;
            const QVector3D worldFront = rayOrigin +
                                         rayDirection * float(nearT);
            const float cameraDepth = -QVector3D::dotProduct(
                worldFront - QVector3D(float(cameraPosition.x),
                                       float(cameraPosition.y),
                                       float(cameraPosition.z)),
                QVector3D(float(viewDirection.x), float(viewDirection.y),
                          float(viewDirection.z)));
            if (!std::isfinite(cameraDepth)) continue;
            RasterWorkbenchVolumeRaySample sample;
            sample.x = x;
            sample.y = y;
            sample.cameraDepth = cameraDepth;
            sample.premultipliedColor = {
                radiance.x(), radiance.y(), radiance.z(), alpha};
            samples->append(sample);
        }
    }
    return !samples->isEmpty();
}

} // namespace classiCAD
