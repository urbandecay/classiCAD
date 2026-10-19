#include "nurbs_solid_face_extrusion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include <QVarLengthArray>

namespace classiCAD {
namespace {
Point3D add(Point3D a, Point3D b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Point3D sub(Point3D a, Point3D b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Point3D mul(Point3D a, double b) { return {a.x*b,a.y*b,a.z*b}; }
double dot(Point3D a, Point3D b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Point3D cross(Point3D a, Point3D b)
{ return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double norm(Point3D a) { return std::hypot(a.x,std::hypot(a.y,a.z)); }
Point3D unit(Point3D a) { const double n=norm(a); return n>0?mul(a,1/n):Point3D{}; }
using Coordinates=std::array<double,3>;
struct Rectangle {
    Coordinates low{},high{};
    int axis=-1;
};
struct BoundaryRectangle {
    Rectangle rectangle;
    int owner=-1;
    int sign=1;
};

SolidFaceExtrusionResult extendConvexPlanarFace(
    const NurbsExtrusionSolid3D &source,int selected,Point3D movement)
{
    SolidFaceExtrusionResult result;
    const auto &faces=source.boundaryFaces;
    double scale=0;
    for (const auto &face:faces) for (Point3D point:face.controlPoints)
        scale=std::max(scale,norm(sub(point,faces[selected].controlPoints[0])));
    const double tolerance=std::max(1.0e-12,scale*1.0e-9);
    for (int i=0;i<faces.size();++i) {
        const auto &cv=faces[i].controlPoints;
        Point3D normal=unit(cross(sub(cv[2],cv[0]),sub(cv[1],cv[0])));
        if (source.boundaryFaceReversed[i]) normal=mul(normal,-1);
        if (i==selected && (dot(movement,normal)<=tolerance ||
            norm(cross(movement,normal))>norm(movement)*1.0e-8)) return result;
        for (const auto &face:faces) for (Point3D point:face.controlPoints)
            if (dot(sub(point,cv[0]),normal)>tolerance) return result;
        for (Point3D point:cv) if (std::abs(dot(sub(point,cv[0]),normal))>tolerance) return result;
    }
    result.solid=source;result.faceIndex=selected;
    for (auto &point:result.solid.boundaryFaces[selected].controlPoints) point=add(point,movement);
    const int forward[]={0,2,3,1},reverse[]={0,1,3,2};
    const int *perimeter=source.boundaryFaceReversed[selected]?reverse:forward;
    for (int i=0;i<4;++i) {
        const auto a=faces[selected].controlPoints[perimeter[i]],
                   b=faces[selected].controlPoints[perimeter[(i+1)%4]];
        NurbsSurface3D wall;
        wall.controlVertexCountU=wall.controlVertexCountV=2;
        wall.controlPoints={a,add(a,movement),b,add(b,movement)};
        wall.weights={1,1,1,1};wall.knotsU=wall.knotsV={0,1};
        result.solid.boundaryFaces.append(wall);result.solid.boundaryFaceReversed.append(false);
    }
    if (!validateNurbsSolid(result.solid)) result.faceIndex=-1;
    return result;
}

SolidFaceExtrusionResult rejected(const char *reason,
    SolidFaceExtrusionError code=SolidFaceExtrusionError::InvalidOperation)
{
    SolidFaceExtrusionResult result;
    result.error=QString::fromLatin1(reason);
    result.errorCode=code;
    return result;
}

// The partition consists exclusively of exact source-face planes and the
// requested end plane. It is an analytic arrangement, not a sampled display
// mesh or a quantized voxel representation. Its boundary is converted back to
// exact degree-one NURBS patches before anything reaches the document.
class OrthogonalSweep {
public:
    OrthogonalSweep(const NurbsExtrusionSolid3D &solid, int selected,
                    Point3D displacement, bool manifold)
        : source_(solid), selected_(selected), selectedIndices_{selected}, movement_(displacement), manifold_(manifold) {}

    OrthogonalSweep(const NurbsExtrusionSolid3D &solid, const QVector<int> &selected,
                    Point3D displacement, bool manifold)
        : source_(solid), selected_(selected.first()), selectedIndices_(selected),
          movement_(displacement), manifold_(manifold) {}

    SolidFaceExtrusionResult run();

private:
    Coordinates local(Point3D p) const {
        p=sub(p,origin_);
        return {dot(p,basis_[0]),dot(p,basis_[1]),dot(p,basis_[2])};
    }
    Point3D world(Coordinates p) const {
        return add(origin_,add(mul(basis_[0],p[0]),
                               add(mul(basis_[1],p[1]),mul(basis_[2],p[2]))));
    }
    bool covers(const Rectangle &r, Coordinates p) const {
        for (int a=0;a<3;++a)
            if (a!=r.axis && (p[a]<r.low[a]-tolerance_ || p[a]>r.high[a]+tolerance_)) return false;
        return true;
    }
    bool inside(Coordinates p) const {
        QVarLengthArray<double,16> hits;
        for (const auto &r:rectangles_)
            if (r.axis==2 && r.low[2]>p[2]+tolerance_ && covers(r,p)) hits.append(r.low[2]);
        // A ray on a coplanar patch seam crosses one boundary plane, even
        // when two or more exact face patches contain that same point.
        std::sort(hits.begin(),hits.end());
        int intersections=0;
        double previous=std::numeric_limits<double>::lowest();
        for (double hit:hits) {
            if (hit-previous>tolerance_) ++intersections;
            previous=hit;
        }
        return intersections%2!=0;
    }
    int offset(int x,int y,int z) const { return (x*counts_[1]+y)*counts_[2]+z; }
    bool occupied(std::array<int,3> cell) const {
        for (int a=0;a<3;++a) if (cell[a]<0 || cell[a]>=counts_[a]) return false;
        return cells_[offset(cell[0],cell[1],cell[2])]!=0;
    }
    int owner(const Rectangle &r,int sign) const;
    NurbsSurface3D surface(const BoundaryRectangle &r) const;

    const NurbsExtrusionSolid3D &source_;
    int selected_;
    QVector<int> selectedIndices_;
    Point3D movement_,origin_;
    std::array<Point3D,3> basis_;
    bool manifold_,mergeWalls_=false;
    double tolerance_=0, target_=0;
    int outward_=1;
    QVector<Rectangle> rectangles_;
    std::array<std::vector<double>,3> planes_;
    std::array<int,3> counts_{};
    std::vector<unsigned char> cells_;
};

int OrthogonalSweep::owner(const Rectangle &r,int sign) const
{
    Coordinates center;
    for (int a=0;a<3;++a) center[a]=(r.low[a]+r.high[a])*0.5;
    if (r.axis==2 && std::abs(center[2]-target_)<=tolerance_ && sign==outward_) {
        for (int selected : selectedIndices_)
            if (covers(rectangles_[selected],center)) return selected;
    }
    for (int i=0;i<rectangles_.size();++i) {
        if (selectedIndices_.contains(i)) continue;
        const auto &old=rectangles_[i];
        if (old.axis==r.axis && std::abs(old.low[r.axis]-center[r.axis])<=tolerance_ &&
            covers(old,center)) return i;
    }
    if (mergeWalls_ && r.axis!=2) {
        // Extend the adjoining coplanar wall, preserving its identity and
        // parameter direction. Unrelated planes never acquire new geometry.
        int nearest=-1;
        double best=std::numeric_limits<double>::max();
        const int transverse=1-r.axis;
        for (int i=0;i<rectangles_.size();++i) {
            if (selectedIndices_.contains(i)) continue;
            const auto &old=rectangles_[i];
            if (old.axis!=r.axis || std::abs(old.low[r.axis]-center[r.axis])>tolerance_ ||
                old.low[2]>tolerance_ || old.high[2]<-tolerance_) continue;
            bool touchesRegion=false;
            for (int selectedIndex:selectedIndices_) {
                const auto &selected=rectangles_[selectedIndex];
                if (std::min(old.high[transverse],selected.high[transverse])-
                    std::max(old.low[transverse],selected.low[transverse])>tolerance_)
                    touchesRegion=true;
            }
            if (!touchesRegion) continue;
            double distance=0;
            for (int a=0;a<3;++a) {
                const double gap=std::max({old.low[a]-center[a],center[a]-old.high[a],0.0});
                distance+=gap*gap;
            }
            if (distance<best) { best=distance; nearest=i; }
        }
        if (nearest>=0) return nearest;
    }
    // Four new perimeter walls, ordered like the selected oriented perimeter.
    const auto &selected=rectangles_[selected_];
    int side=r.axis==1 ? (std::abs(center[1]-selected.low[1])<=tolerance_?0:2)
                       : (std::abs(center[0]-selected.high[0])<=tolerance_?1:3);
    if (outward_<0) side=3-side;
    return rectangles_.size()+side;
}

NurbsSurface3D OrthogonalSweep::surface(const BoundaryRectangle &boundary) const
{
    const auto &r=boundary.rectangle;
    const int u=(r.axis+1)%3, v=(r.axis+2)%3;
    Coordinates p=r.low;
    QVector<Point3D> corners;
    for (int i=0;i<2;++i) for (int j=0;j<2;++j) {
        p[u]=i?r.high[u]:r.low[u];
        p[v]=j?r.high[v]:r.low[v];
        corners.append(world(p));
    }
    NurbsSurface3D result;
    result.controlVertexCountU=result.controlVertexCountV=2;
    result.weights={1,1,1,1};
    result.knotsU=result.knotsV={0,1};
    result.controlPoints=corners;
    if (boundary.owner>=rectangles_.size()) return result;

    result=source_.boundaryFaces[boundary.owner];
    Point3D original=result.controlPoints[0];
    if (selectedIndices_.contains(boundary.owner)) original=add(original,movement_);
    const Point3D du=sub(result.controlPoints[2],result.controlPoints[0]);
    const Point3D dv=sub(result.controlPoints[1],result.controlPoints[0]);
    double u0=std::numeric_limits<double>::max(),u1=-u0,v0=u0,v1=-u0;
    for (Point3D corner:corners) {
        const double a=dot(sub(corner,original),du)/dot(du,du);
        const double b=dot(sub(corner,original),dv)/dot(dv,dv);
        u0=std::min(u0,a); u1=std::max(u1,a);
        v0=std::min(v0,b); v1=std::max(v1,b);
    }
    const auto at=[&](double a,double b) { return add(original,add(mul(du,a),mul(dv,b))); };
    result.controlPoints={at(u0,v0),at(u0,v1),at(u1,v0),at(u1,v1)};
    const auto trimDomain=[](QVector<qreal> &knots,double first,double last) {
        const double start=knots[0],span=knots[1]-start;
        // Resizing retains the source domain; genuine interior subdivision
        // retains the corresponding subdomain instead of resetting to [0,1].
        const double a=std::clamp(first,0.0,1.0),b=std::clamp(last,0.0,1.0);
        if (b-a>1.0e-12) knots={start+span*a,start+span*b};
    };
    trimDomain(result.knotsU,u0,u1); trimDomain(result.knotsV,v0,v1);
    return result;
}

SolidFaceExtrusionResult OrthogonalSweep::run()
{
    const auto &selected=source_.boundaryFaces[selected_];
    origin_=selected.controlPoints[0];
    basis_[0]=unit(sub(selected.controlPoints[2],origin_));
    basis_[2]=unit(cross(basis_[0],sub(selected.controlPoints[1],origin_)));
    basis_[1]=cross(basis_[2],basis_[0]);
    double scale=0;
    for (const auto &face:source_.boundaryFaces) for (Point3D p:face.controlPoints)
        scale=std::max(scale,norm(sub(p,origin_)));
    tolerance_=std::max(1.0e-12,scale*1.0e-9);
    target_=dot(movement_,basis_[2]);
    if (norm(movement_)<=tolerance_) return rejected("Move the endpoint to set the extrusion distance");
    if (!std::isfinite(target_) || norm(sub(movement_,mul(basis_[2],target_)))>
        std::max(tolerance_,norm(movement_)*1.0e-8))
        return rejected("Move the selected face along its normal");

    for (const auto &face:source_.boundaryFaces) {
        Rectangle r;
        r.low.fill(std::numeric_limits<double>::max());
        r.high.fill(std::numeric_limits<double>::lowest());
        QVector<Coordinates> cv;
        for (Point3D p:face.controlPoints) {
            cv.append(local(p));
            for (int a=0;a<3;++a) { r.low[a]=std::min(r.low[a],cv.last()[a]); r.high[a]=std::max(r.high[a],cv.last()[a]); }
        }
        int constant=0;
        for (int a=0;a<3;++a) if (r.high[a]-r.low[a]<=tolerance_) { r.axis=a; ++constant; }
        if (constant!=1) return rejected("Connected extrusion requires mutually orthogonal rectangular planar faces",
                                       SolidFaceExtrusionError::UnsupportedBoundary);
        for (const auto &p:cv) for (int a=0;a<3;++a)
            if (std::abs(p[a]-r.low[a])>tolerance_ && std::abs(p[a]-r.high[a])>tolerance_)
                return rejected("Connected extrusion requires rectangular face boundaries",
                                SolidFaceExtrusionError::UnsupportedBoundary);
        const auto natural=cross(sub(face.controlPoints[2],face.controlPoints[0]),
                                 sub(face.controlPoints[1],face.controlPoints[0]));
        if (norm(natural)<=tolerance_*tolerance_) return rejected("The source contains a collapsed face");
        if (norm(sub(add(face.controlPoints[1],face.controlPoints[2]),
                         add(face.controlPoints[0],face.controlPoints[3])))>tolerance_)
            return rejected("Connected extrusion requires rectangular face boundaries",
                            SolidFaceExtrusionError::UnsupportedBoundary);
        rectangles_.append(r);
        for (int a=0;a<3;++a) { planes_[a].push_back(r.low[a]); planes_[a].push_back(r.high[a]); }
    }
    for (int index : selectedIndices_) {
        const auto &r = rectangles_[index];
        if (r.axis != 2 || std::abs(r.low[2]) > tolerance_)
            return rejected("Select coplanar faces for a shared region extrusion");
        const auto &cv = source_.boundaryFaces[index].controlPoints;
        Point3D normal = unit(cross(sub(cv[2],cv[0]),sub(cv[1],cv[0])));
        if (source_.boundaryFaceReversed[index]) normal = mul(normal,-1);
        const auto &first = source_.boundaryFaces[selected_].controlPoints;
        Point3D firstNormal = unit(cross(sub(first[2],first[0]),sub(first[1],first[0])));
        if (source_.boundaryFaceReversed[selected_]) firstNormal = mul(firstNormal,-1);
        if (dot(normal,firstNormal) < 1.0 - 1.0e-8)
            return rejected("Selected region faces must have matching outward normals");
    }
    Coordinates center;
    for (int a=0;a<3;++a) center[a]=(rectangles_[selected_].low[a]+rectangles_[selected_].high[a])*0.5;
    // Infer solid side from the closed boundary, rather than trusting winding
    // flags left by earlier edits. This also repairs old reversed step walls.
    center[2]=tolerance_*4;
    outward_=inside(center)?-1:1;
    mergeWalls_=manifold_ || target_*outward_<0;
    if (!mergeWalls_) {
        for (const auto &r:rectangles_) {
            if (r.axis==2) continue;
            const int transverse=1-r.axis;
            const auto &s=rectangles_[selected_];
            const bool onSide=std::abs(r.low[r.axis]-s.low[r.axis])<=tolerance_ ||
                              std::abs(r.low[r.axis]-s.high[r.axis])<=tolerance_;
            if (onSide && std::min(r.high[transverse],s.high[transverse])-
                std::max(r.low[transverse],s.low[transverse])>tolerance_ &&
                r.low[2]<=tolerance_ && r.high[2]>=-tolerance_ &&
                (outward_>0?r.high[2]>tolerance_:r.low[2]<-tolerance_)) mergeWalls_=true;
        }
    }
    planes_[2].push_back(target_);
    qint64 cellCount=1;
    for (int a=0;a<3;++a) {
        auto &values=planes_[a];
        std::sort(values.begin(),values.end());
        values.erase(std::unique(values.begin(),values.end(),[&](double p,double q) {
            return std::abs(p-q)<=tolerance_;
        }),values.end());
        counts_[a]=static_cast<int>(values.size())-1;
        cellCount*=counts_[a];
    }
    if (cellCount<=0 || cellCount>2000000)
        return rejected("The face arrangement exceeds the connected extrusion complexity limit");
    cells_.resize(static_cast<size_t>(cellCount));
    for (int x=0;x<counts_[0];++x) for (int y=0;y<counts_[1];++y) for (int z=0;z<counts_[2];++z) {
        const Coordinates p{(planes_[0][x]+planes_[0][x+1])*0.5,
                            (planes_[1][y]+planes_[1][y+1])*0.5,
                            (planes_[2][z]+planes_[2][z+1])*0.5};
        bool filled=inside(p);
        if (p[2]>std::min(0.0,target_) && p[2]<std::max(0.0,target_)) {
            for (int index : selectedIndices_) if (covers(rectangles_[index],p)) {
                filled=target_*outward_>0;
                break;
            }
        }
        cells_[offset(x,y,z)]=filled;
    }

    QVector<BoundaryRectangle> patches;
    // A boundary cell exists iff exactly one side contains material. This
    // single rule handles extension, recession, flush walls and wall reversal.
    for (int axis=0;axis<3;++axis) {
        const int u=(axis+1)%3,v=(axis+2)%3;
        for (int plane=0;plane<=counts_[axis];++plane) {
            std::vector<int> owners(counts_[u]*counts_[v],-1),signs(owners.size(),0);
            for (int i=0;i<counts_[u];++i) for (int j=0;j<counts_[v];++j) {
                std::array<int,3> cell{}; cell[axis]=plane-1;cell[u]=i;cell[v]=j;
                const bool before=occupied(cell); ++cell[axis]; const bool after=occupied(cell);
                if (before==after) continue;
                Rectangle r; r.axis=axis;r.low[axis]=r.high[axis]=planes_[axis][plane];
                r.low[u]=planes_[u][i];r.high[u]=planes_[u][i+1];
                r.low[v]=planes_[v][j];r.high[v]=planes_[v][j+1];
                const int slot=i*counts_[v]+j,sign=before?1:-1;
                owners[slot]=owner(r,sign); signs[slot]=sign;
            }
            for (int i=0;i<counts_[u];++i) for (int j=0;j<counts_[v];++j) {
                const int slot=i*counts_[v]+j,label=owners[slot],sign=signs[slot];
                if (label<0) continue;
                int endV=j+1;
                while (endV<counts_[v] && owners[i*counts_[v]+endV]==label && signs[i*counts_[v]+endV]==sign) ++endV;
                int endU=i+1;
                while (endU<counts_[u]) {
                    bool same=true;
                    for (int k=j;k<endV;++k) same&=owners[endU*counts_[v]+k]==label && signs[endU*counts_[v]+k]==sign;
                    if (!same) break;
                    ++endU;
                }
                Rectangle r;r.axis=axis;r.low[axis]=r.high[axis]=planes_[axis][plane];
                r.low[u]=planes_[u][i];r.high[u]=planes_[u][endU];r.low[v]=planes_[v][j];r.high[v]=planes_[v][endV];
                patches.append({r,label,sign});
                for (int a=i;a<endU;++a) for (int b=j;b<endV;++b) owners[a*counts_[v]+b]=-1;
            }
        }
    }
    // Keep each original face's first surviving patch in source order, then
    // append subdivisions/new walls. The explicit moving-face mapping handles
    // any removed wall before the selected slot.
    std::stable_sort(patches.begin(),patches.end(),[](const auto &a,const auto &b) { return a.owner<b.owner; });
    QVector<BoundaryRectangle> ordered,extras;
    int previous=-1;
    for (const auto &patch:patches) {
        if (patch.owner==previous) extras.append(patch); else ordered.append(patch);
        previous=patch.owner;
    }
    ordered+=extras;
    SolidFaceExtrusionResult result;
    result.solid=source_;
    result.solid.boundaryFaces.clear();result.solid.boundaryFaceReversed.clear();
    QVector<int> movingCounts(selectedIndices_.size(),0);
    result.faceIndices.fill(-1,selectedIndices_.size());
    for (const auto &patch:ordered) {
        NurbsSurface3D face=surface(patch);
        const int selectedSlot=selectedIndices_.indexOf(patch.owner);
        if (selectedSlot>=0) {
            result.faceIndices[selectedSlot]=result.solid.boundaryFaces.size();
            ++movingCounts[selectedSlot];
        }
        const auto normal=cross(sub(face.controlPoints[2],face.controlPoints[0]),sub(face.controlPoints[1],face.controlPoints[0]));
        result.solid.boundaryFaceReversed.append(dot(normal,basis_[patch.rectangle.axis])*patch.sign<0);
        result.solid.boundaryFaces.append(std::move(face));
    }
    result.faceIndex=result.faceIndices.first();
    for (int slot=0;slot<selectedIndices_.size();++slot) {
        if (movingCounts[slot]!=1) return rejected("The requested extrusion reaches or crosses an opposing boundary");
        const auto &moving=result.solid.boundaryFaces[result.faceIndices[slot]];
        const auto &original=source_.boundaryFaces[selectedIndices_[slot]];
        for (int i=0;i<4;++i) if (norm(sub(moving.controlPoints[i],add(original.controlPoints[i],movement_)))>tolerance_*4)
            return rejected("The requested extrusion would split or remove a selected face");
    }
    if (!validateNurbsSolid(result.solid)) return rejected("The extrusion could not produce a closed NURBS boundary");
    return result;
}
} // namespace

SolidFaceExtrusionResult planNurbsSolidFaceExtrusion(
    const NurbsExtrusionSolid3D &solid,int faceIndex,const Point3D &displacement,bool manifold)
{
    if (!validateNurbsSolid(solid)) return rejected("The source solid has an invalid boundary");
    const auto faces=nurbsSolidFaces(solid);
    if (faceIndex<0 || faceIndex>=faces.size()) return rejected("The selected solid face no longer exists");
    // A trimmed planar cap can resize its exact translational construction
    // without turning curved or trimmed walls into approximate planar patches.
    if (!manifold || !solid.boundaryFaces.isEmpty() || faceIndex>1) {
        NurbsExtrusionSolid3D boundary=solid;
        if (!materializeNurbsSolidBoundary(&boundary))
            return rejected("Connected extrusion requires rectangular planar boundary patches");
        auto result=OrthogonalSweep(boundary,faceIndex,displacement,manifold).run();
        if (!result.succeeded() && !manifold &&
            result.errorCode==SolidFaceExtrusionError::UnsupportedBoundary) {
            auto extension=extendConvexPlanarFace(boundary,faceIndex,displacement);
            if (extension.succeeded()) return extension;
        }
        return result;
    }
    WorkPlaneFrame frame;
    if (!nurbsSolidBaseFrame(solid.baseSurface,&frame)) return rejected("The cap is not planar");
    if (norm(displacement)<=1.0e-12 ||
        norm(cross(displacement,frame.normal))>norm(displacement)*1.0e-8 ||
        norm(cross(solid.displacement,frame.normal))>norm(solid.displacement)*1.0e-8)
        return rejected("Move the planar cap along the solid's normal");
    SolidFaceExtrusionResult result;
    result.solid=solid;result.faceIndex=faceIndex;
    if (faceIndex==1) result.solid.displacement=add(solid.displacement,displacement);
    else {
        for (auto &point:result.solid.baseSurface.controlPoints) point=add(point,displacement);
        result.solid.displacement=sub(solid.displacement,displacement);
    }
    if (dot(result.solid.displacement,solid.displacement)<=0 || !validateNurbsSolid(result.solid))
        return rejected("The requested extrusion reaches or crosses the opposite cap");
    return result;
}

SolidFaceExtrusionResult planNurbsSolidRegionExtrusion(
    const NurbsExtrusionSolid3D &solid,const QVector<int> &faceIndices,
    const Point3D &displacement,bool manifold)
{
    if (faceIndices.isEmpty()) return rejected("Select at least one solid face");
    if (faceIndices.size()==1) {
        auto result=planNurbsSolidFaceExtrusion(solid,faceIndices.first(),displacement,manifold);
        if (result.succeeded()) result.faceIndices={result.faceIndex};
        return result;
    }
    if (!validateNurbsSolid(solid)) return rejected("The source solid has an invalid boundary");
    NurbsExtrusionSolid3D boundary=solid;
    if (!materializeNurbsSolidBoundary(&boundary))
        return rejected("Connected region extrusion requires rectangular planar boundary patches");
    QVector<int> unique;
    for (int index:faceIndices) {
        if (index<0 || index>=boundary.boundaryFaces.size())
            return rejected("A selected solid face no longer exists");
        if (unique.contains(index)) return rejected("A region face was selected more than once");
        unique.append(index);
    }
    return OrthogonalSweep(boundary,unique,displacement,manifold).run();
}

bool extrudeNurbsSolidFace(const NurbsExtrusionSolid3D &solid,int faceIndex,
                          const Point3D &displacement,NurbsExtrusionSolid3D *result,
                          bool manifold,int *resultFaceIndex)
{
    if (!result) return false;
    auto candidate=planNurbsSolidFaceExtrusion(solid,faceIndex,displacement,manifold);
    if (!candidate.succeeded()) return false;
    if (resultFaceIndex) *resultFaceIndex=candidate.faceIndex;
    *result=std::move(candidate.solid);
    return true;
}
} // namespace classiCAD
