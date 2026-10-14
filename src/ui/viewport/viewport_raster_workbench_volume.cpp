/* SPDX-FileCopyrightText: 2026 classiCAD contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "viewport_raster_workbench_volume.h"

#include "core/geometry/scalar_volume.h"

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
    // Blender v5.2.2 workbench volume pass derives slice opacity step from
    // the world AABB extent on the selected grid axis, times 0.05.
    const ScalarVolumeMatrix &m=v.gridToObject;
    const ScalarVolumeMatrix &n=v.objectToWorld;
    auto worldFor=[&](Vec3 g){Vec3 q=mapPoint(m,g); q=mapPoint(n,q); q.x+=object.placementTranslation.x; q.y+=object.placementTranslation.y; q.z+=object.placementTranslation.z; return q;};
    Vec3 low{std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),std::numeric_limits<double>::max()};
    Vec3 high{-low.x,-low.y,-low.z};
    for(int i=0;i<8;++i){Vec3 g{(i&1)?maxs[0]:mins[0],(i&2)?maxs[1]:mins[1],(i&4)?maxs[2]:mins[2]};Vec3 w=worldFor(g);if(!finite(w))return false;low.x=std::min(low.x,w.x);low.y=std::min(low.y,w.y);low.z=std::min(low.z,w.z);high.x=std::max(high.x,w.x);high.y=std::max(high.y,w.y);high.z=std::max(high.z,w.z);}
    const double ext[3]={high.x-low.x,high.y-low.y,high.z-low.z};
    const float step=float(std::max(1e-16,ext[axis]*0.05));
    std::array<Vertex,4> vertices;
    const Point3D eye=camera.cameraPosition(viewportSize), forward=camera.viewDirection();
    for(int i=0;i<4;++i){Vec3 w=worldFor(grid[i]);if(!finite(w))return false;QPointF s;if(!camera.worldPointToScreenUnclipped({w.x,w.y,w.z},viewportSize,&s))return false;double d=-((w.x-eye.x)*forward.x+(w.y-eye.y)*forward.y+(w.z-eye.z)*forward.z);double projectionDepth=camera.isPerspectiveEnabled()?d:1.0;if(!std::isfinite(d)||projectionDepth<=1e-15)return false;vertices[i]={s,grid[i],d,projectionDepth};}
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
    const std::array<Vertex,3> t0{vertices[0],vertices[1],vertices[3]}, t1{vertices[0],vertices[3],vertices[2]};
    if(!rasterTriangle(t0,devicePixelRatio,int(width),int(height),dense,nx,ny,nz,v,samples,&seen,step)||
       !rasterTriangle(t1,devicePixelRatio,int(width),int(height),dense,nx,ny,nz,v,samples,&seen,step)){samples->clear();return false;}
    return !samples->isEmpty();
}

} // namespace classiCAD
