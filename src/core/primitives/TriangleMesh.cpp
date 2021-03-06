#include "TriangleMesh.hpp"
#include "EmbreeUtil.hpp"

#include "sampling/SampleGenerator.hpp"
#include "sampling/SampleWarp.hpp"

#include "math/TangentFrame.hpp"
#include "math/Mat4f.hpp"
#include "math/Vec.hpp"
#include "math/Box.hpp"

#include "io/JsonSerializable.hpp"
#include "io/JsonUtils.hpp"
#include "io/MeshIO.hpp"
#include "io/Scene.hpp"

#include <rapidjson/document.h>
#include <unordered_map>
#include <iostream>

namespace Tungsten {

struct MeshIntersection
{
    Vec3f Ng;
    float u;
    float v;
    int id0;
    int id1;
    bool backSide;
};

TriangleMesh::TriangleMesh()
: _smoothed(false),
  _backfaceCulling(false)
{
}

TriangleMesh::TriangleMesh(const TriangleMesh &o)
: Primitive(o),
  _path(o._path),
  _smoothed(o._smoothed),
  _backfaceCulling(o._backfaceCulling),
  _verts(o._verts),
  _tris(o._tris),
  _bounds(o._bounds)
{
}

TriangleMesh::TriangleMesh(std::vector<Vertex> verts, std::vector<TriangleI> tris,
             const std::shared_ptr<Bsdf> &bsdf,
             const std::string &name, bool smoothed, bool backfaceCull)
: TriangleMesh(
      std::move(verts),
      std::move(tris),
      std::vector<std::shared_ptr<Bsdf>>(1, bsdf),
      name,
      smoothed,
      backfaceCull
  )
{
}

TriangleMesh::TriangleMesh(std::vector<Vertex> verts, std::vector<TriangleI> tris,
             std::vector<std::shared_ptr<Bsdf>> bsdfs,
             const std::string &name, bool smoothed, bool backfaceCull)
: Primitive(name),
  _path(std::make_shared<Path>(std::string(name).append(".wo3"))),
  _smoothed(smoothed),
  _backfaceCulling(backfaceCull),
  _verts(std::move(verts)),
  _tris(std::move(tris)),
  _bsdfs(std::move(bsdfs))
{
    _path->freezeWorkingDirectory();
}

Vec3f TriangleMesh::unnormalizedGeometricNormalAt(int triangle) const
{
    const TriangleI &t = _tris[triangle];
    Vec3f p0 = _tfVerts[t.v0].pos();
    Vec3f p1 = _tfVerts[t.v1].pos();
    Vec3f p2 = _tfVerts[t.v2].pos();
    return (p1 - p0).cross(p2 - p0);
}

Vec3f TriangleMesh::normalAt(int triangle, float u, float v) const
{
    const TriangleI &t = _tris[triangle];
    Vec3f n0 = _tfVerts[t.v0].normal();
    Vec3f n1 = _tfVerts[t.v1].normal();
    Vec3f n2 = _tfVerts[t.v2].normal();
    return ((1.0f - u - v)*n0 + u*n1 + v*n2).normalized();
}

Vec2f TriangleMesh::uvAt(int triangle, float u, float v) const
{
    const TriangleI &t = _tris[triangle];
    Vec2f uv0 = _tfVerts[t.v0].uv();
    Vec2f uv1 = _tfVerts[t.v1].uv();
    Vec2f uv2 = _tfVerts[t.v2].uv();
    return (1.0f - u - v)*uv0 + u*uv1 + v*uv2;
}

void TriangleMesh::fromJson(const rapidjson::Value &v, const Scene &scene)
{
    Primitive::fromJson(v, scene);

    _path = scene.fetchResource(v, "file");
    JsonUtils::fromJson(v, "smooth", _smoothed);
    JsonUtils::fromJson(v, "backface_culling", _backfaceCulling);

    const rapidjson::Value::Member *bsdf = v.FindMember("bsdf");
    if (bsdf && bsdf->value.IsArray()) {
        if (bsdf->value.Size() == 0)
            FAIL("Empty BSDF array for triangle mesh");
        for (int i = 0; i < int(bsdf->value.Size()); ++i)
            _bsdfs.emplace_back(scene.fetchBsdf(bsdf->value[i]));
    } else {
        _bsdfs.emplace_back(scene.fetchBsdf(JsonUtils::fetchMember(v, "bsdf")));
    }
}

rapidjson::Value TriangleMesh::toJson(Allocator &allocator) const
{
    rapidjson::Value v = Primitive::toJson(allocator);
    v.AddMember("type", "mesh", allocator);
    if (_path)
        v.AddMember("file", _path->asString().c_str(), allocator);
    v.AddMember("smooth", _smoothed, allocator);
    v.AddMember("backface_culling", _backfaceCulling, allocator);
    if (_bsdfs.size() == 1) {
        JsonUtils::addObjectMember(v, "bsdf", *_bsdfs[0], allocator);
    } else {
        rapidjson::Value a(rapidjson::kArrayType);
        for (const auto &bsdf : _bsdfs)
            a.PushBack(bsdf->toJson(allocator), allocator);
        v.AddMember("bsdf", std::move(a), allocator);
    }
    return std::move(v);
}

void TriangleMesh::loadResources()
{
    if (_path && !MeshIO::load(*_path, _verts, _tris))
        DBG("Unable to load triangle mesh at %s", *_path);
}

void TriangleMesh::saveResources()
{
    if (_path)
        MeshIO::save(*_path, _verts, _tris);
}

void TriangleMesh::saveAsObj(const Path &path) const
{
    MeshIO::save(path, _verts, _tris);
}

void TriangleMesh::calcSmoothVertexNormals()
{
    static CONSTEXPR float SplitLimit = std::cos(PI*0.15f);
    //static CONSTEXPR float SplitLimit = -1.0f;

    std::vector<Vec3f> geometricN(_verts.size(), Vec3f(0.0f));
    std::unordered_multimap<Vec3f, uint32> posToVert;

    for (uint32 i = 0; i < _verts.size(); ++i) {
        _verts[i].normal() = Vec3f(0.0f);
        posToVert.insert(std::make_pair(_verts[i].pos(), i));
    }

    for (TriangleI &t : _tris) {
        const Vec3f &p0 = _verts[t.v0].pos();
        const Vec3f &p1 = _verts[t.v1].pos();
        const Vec3f &p2 = _verts[t.v2].pos();
        Vec3f normal = (p1 - p0).cross(p2 - p0);
        if (normal == 0.0f)
            normal = Vec3f(0.0f, 1.0f, 0.0f);
        else
            normal.normalize();

        for (int i = 0; i < 3; ++i) {
            Vec3f &n = geometricN[t.vs[i]];
            if (n == 0.0f) {
                n = normal;
            } else if (n.dot(normal) < SplitLimit) {
                _verts.push_back(_verts[t.vs[i]]);
                geometricN.push_back(normal);
                t.vs[i] = _verts.size() - 1;
            }
        }
    }

    for (TriangleI &t : _tris) {
        const Vec3f &p0 = _verts[t.v0].pos();
        const Vec3f &p1 = _verts[t.v1].pos();
        const Vec3f &p2 = _verts[t.v2].pos();
        Vec3f normal = (p1 - p0).cross(p2 - p0);
        Vec3f nN = normal.normalized();

        for (int i = 0; i < 3; ++i) {
            auto iters = posToVert.equal_range(_verts[t.vs[i]].pos());

            for (auto t = iters.first; t != iters.second; ++t)
                if (geometricN[t->second].dot(nN) >= SplitLimit)
                    _verts[t->second].normal() += normal;
        }
    }

    for (uint32 i = 0; i < _verts.size(); ++i) {
        if (_verts[i].normal() == 0.0f)
            _verts[i].normal() = geometricN[i];
        else
            _verts[i].normal().normalize();
    }
}

void TriangleMesh::computeBounds()
{
    Box3f box;
    for (Vertex &v : _verts)
        box.grow(_transform*v.pos());
    _bounds = box;
}

void TriangleMesh::makeCube()
{
    const Vec3f verts[6][4] = {
        {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f, -0.5f}},
        {{-0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f}},
        {{-0.5f,  0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}},
        {{ 0.5f,  0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f}},
        {{-0.5f,  0.5f,  0.5f}, {-0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f}},
        {{ 0.5f,  0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}},
    };
    const Vec2f uvs[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    for (int i = 0; i < 6; ++i) {
        int idx = _verts.size();
        _tris.emplace_back(idx, idx + 2, idx + 1);
        _tris.emplace_back(idx, idx + 3, idx + 2);

        for (int j = 0; j < 4; ++j)
            _verts.emplace_back(verts[i][j], uvs[j]);
    }
}

void TriangleMesh::makeSphere(float radius)
{
    CONSTEXPR int SubDiv = 10;
    CONSTEXPR int Skip = SubDiv*2 + 1;
    for (int f = 0, idx = _verts.size(); f < 3; ++f) {
        for (int s = -1; s <= 1; s += 2) {
            for (int u = -SubDiv; u <= SubDiv; ++u) {
                for (int v = -SubDiv; v <= SubDiv; ++v, ++idx) {
                    Vec3f p(0.0f);
                    p[f] = s;
                    p[(f + 1) % 3] = u*(1.0f/SubDiv)*s;
                    p[(f + 2) % 3] = v*(1.0f/SubDiv);
                    _verts.emplace_back(p.normalized()*radius);

                    if (v > -SubDiv && u > -SubDiv) {
                        _tris.emplace_back(idx - Skip - 1, idx, idx - Skip);
                        _tris.emplace_back(idx - Skip - 1, idx - 1, idx);
                    }
                }
            }
        }
    }
}

void TriangleMesh::makeCone(float radius, float height)
{
    CONSTEXPR int SubDiv = 36;
    int base = _verts.size();
    _verts.emplace_back(Vec3f(0.0f));
    for (int i = 0; i < SubDiv; ++i) {
        float a = i*TWO_PI/SubDiv;
        _verts.emplace_back(Vec3f(std::cos(a)*radius, height, std::sin(a)*radius));
        _tris.emplace_back(base, base + i + 1, base + ((i + 1) % SubDiv) + 1);
    }
}

bool TriangleMesh::intersect(Ray &ray, IntersectionTemporary &data) const
{
    embree::Ray eRay(EmbreeUtil::convert(ray));
    _intersector->intersect(eRay);
    if (eRay && eRay.tfar < ray.farT()) {
        ray.setFarT(eRay.tfar);

        data.primitive = this;
        MeshIntersection *isect = data.as<MeshIntersection>();
        isect->Ng = unnormalizedGeometricNormalAt(eRay.id0);
        isect->u = eRay.u;
        isect->v = eRay.v;
        isect->id0 = eRay.id0;
        isect->id1 = eRay.id1;
        isect->backSide = isect->Ng.dot(ray.dir()) > 0.0f;

        return true;
    }
    return false;
}

bool TriangleMesh::occluded(const Ray &ray) const
{
    embree::Ray eRay(EmbreeUtil::convert(ray));
    return _intersector->occluded(eRay);
}

void TriangleMesh::intersectionInfo(const IntersectionTemporary &data, IntersectionInfo &info) const
{
    const MeshIntersection *isect = data.as<MeshIntersection>();
    info.Ng = isect->Ng.normalized();
    if (_smoothed)
        info.Ns = normalAt(isect->id0, isect->u, isect->v);
    else
        info.Ns = info.Ng;
    info.uv = uvAt(isect->id0, isect->u, isect->v);
    info.primitive = this;
    info.bsdf = _bsdfs[_tris[isect->id0].material].get();
}

bool TriangleMesh::hitBackside(const IntersectionTemporary &data) const
{
    return data.as<MeshIntersection>()->backSide;
}

bool TriangleMesh::tangentSpace(const IntersectionTemporary &data, const IntersectionInfo &/*info*/,
        Vec3f &T, Vec3f &B) const
{
    const MeshIntersection *isect = data.as<MeshIntersection>();
    const TriangleI &t = _tris[isect->id0];
    Vec3f p0 = _tfVerts[t.v0].pos();
    Vec3f p1 = _tfVerts[t.v1].pos();
    Vec3f p2 = _tfVerts[t.v2].pos();
    Vec2f uv0 = _tfVerts[t.v0].uv();
    Vec2f uv1 = _tfVerts[t.v1].uv();
    Vec2f uv2 = _tfVerts[t.v2].uv();
    Vec3f q1 = p1 - p0;
    Vec3f q2 = p2 - p0;
    float s1 = uv1.x() - uv0.x(), t1 = uv1.y() - uv0.y();
    float s2 = uv2.x() - uv0.x(), t2 = uv2.y() - uv0.y();
    float invDet = s1*t2 - s2*t1;
    if (std::abs(invDet) < 1e-6f)
        return false;
    float det = 1.0f/invDet;
    T = det*(q1*t2 - t1*q2);
    B = det*(q2*s1 - s2*q1);

    return true;
}

const TriangleMesh &TriangleMesh::asTriangleMesh()
{
    return *this;
}

bool TriangleMesh::isSamplable() const
{
    return true;
}

void TriangleMesh::makeSamplable(uint32 /*threadIndex*/)
{
    if (_triSampler)
        return;

    std::vector<float> areas(_tris.size());
    _totalArea = 0.0f;
    for (size_t i = 0; i < _tris.size(); ++i) {
        Vec3f p0 = _tfVerts[_tris[i].v0].pos();
        Vec3f p1 = _tfVerts[_tris[i].v1].pos();
        Vec3f p2 = _tfVerts[_tris[i].v2].pos();
        areas[i] = MathUtil::triangleArea(p0, p1, p2);
        _totalArea += areas[i];
    }
    _triSampler.reset(new Distribution1D(std::move(areas)));
}

float TriangleMesh::inboundPdf(uint32 /*threadIndex*/, const IntersectionTemporary &/*data*/,
        const IntersectionInfo &info, const Vec3f &p, const Vec3f &d) const
{
    return (p - info.p).lengthSq()/(-d.dot(info.Ng.normalized())*_totalArea);
}

bool TriangleMesh::sampleInboundDirection(uint32 /*threadIndex*/, LightSample &sample) const
{
    float u = sample.sampler->next1D();
    int idx;
    _triSampler->warp(u, idx);

    Vec3f p0 = _tfVerts[_tris[idx].v0].pos();
    Vec3f p1 = _tfVerts[_tris[idx].v1].pos();
    Vec3f p2 = _tfVerts[_tris[idx].v2].pos();
    Vec3f normal = (p1 - p0).cross(p2 - p0).normalized();

    Vec3f p = SampleWarp::uniformTriangle(sample.sampler->next2D(), p0, p1, p2);
    Vec3f L = p - sample.p;

    float rSq = L.lengthSq();
    sample.dist = std::sqrt(rSq);
    sample.d = L/sample.dist;
    float cosTheta = -(normal.dot(sample.d));
    if (cosTheta <= 0.0f)
        return false;
    sample.pdf = rSq/(cosTheta*_totalArea);

    return true;
}

bool TriangleMesh::sampleOutboundDirection(uint32 /*threadIndex*/, LightSample &sample) const
{
    float u = sample.sampler->next1D();
    int idx;
    _triSampler->warp(u, idx);

    Vec3f p0 = _tfVerts[_tris[idx].v0].pos();
    Vec3f p1 = _tfVerts[_tris[idx].v1].pos();
    Vec3f p2 = _tfVerts[_tris[idx].v2].pos();
    Vec3f normal = (p1 - p0).cross(p2 - p0).normalized();
    TangentFrame frame(normal);

    sample.p = SampleWarp::uniformTriangle(sample.sampler->next2D(), p0, p1, p2);
    sample.d = SampleWarp::cosineHemisphere(sample.sampler->next2D());
    sample.pdf = SampleWarp::cosineHemispherePdf(sample.d)/_totalArea;
    sample.d = frame.toGlobal(sample.d);

    return true;
}

bool TriangleMesh::invertParametrization(Vec2f /*uv*/, Vec3f &/*pos*/) const
{
    return false;
}

bool TriangleMesh::isDelta() const
{
    return _verts.empty() || _tris.empty();
}

bool TriangleMesh::isInfinite() const
{
    return false;
}

// Questionable, but there is no cheap and realiable way to compute this factor
float TriangleMesh::approximateRadiance(uint32 /*threadIndex*/, const Vec3f &/*p*/) const
{
    return -1.0f;
}

Box3f TriangleMesh::bounds() const
{
    return _bounds;
}

void TriangleMesh::prepareForRender()
{
    computeBounds();

    if (_verts.empty() || _tris.empty())
        return;

    _geom = embree::rtcNewTriangleMesh(_tris.size(), _verts.size(), "bvh2");
    embree::RTCVertex   *vs = embree::rtcMapPositionBuffer(_geom);
    embree::RTCTriangle *ts = embree::rtcMapTriangleBuffer(_geom);

    for (size_t i = 0; i < _tris.size(); ++i) {
        const TriangleI &t = _tris[i];
        _tris[i].material = clamp(_tris[i].material, 0, int(_bsdfs.size()) - 1);
        ts[i] = embree::RTCTriangle(t.v0, t.v1, t.v2, i, 0);
    }

    _tfVerts.resize(_verts.size());
    Mat4f normalTform(_transform.toNormalMatrix());
    for (size_t i = 0; i < _verts.size(); ++i) {
        _tfVerts[i] = Vertex(
            _transform*_verts[i].pos(),
            normalTform.transformVector(_verts[i].normal()),
            _verts[i].uv()
        );
        const Vec3f &p = _tfVerts[i].pos();
        vs[i] = embree::RTCVertex(p.x(), p.y(), p.z());
    }

    _totalArea = 0.0f;
    for (size_t i = 0; i < _tris.size(); ++i) {
        Vec3f p0 = _tfVerts[_tris[i].v0].pos();
        Vec3f p1 = _tfVerts[_tris[i].v1].pos();
        Vec3f p2 = _tfVerts[_tris[i].v2].pos();
        _totalArea += MathUtil::triangleArea(p0, p1, p2);
    }

    embree::rtcUnmapPositionBuffer(_geom);
    embree::rtcUnmapTriangleBuffer(_geom);

    embree::rtcBuildAccel(_geom, "objectsplit");
    if (_backfaceCulling)
        _intersector = embree::rtcQueryIntersector1(_geom, "fast.moeller_cull");
    else
        _intersector = embree::rtcQueryIntersector1(_geom, "fast.moeller");
}

void TriangleMesh::cleanupAfterRender()
{
    if (_geom)
        embree::rtcDeleteGeometry(_geom);
    _geom = nullptr;
    _intersector = nullptr;
    _tfVerts.clear();
}

int TriangleMesh::numBsdfs() const
{
    return _bsdfs.size();
}

std::shared_ptr<Bsdf> &TriangleMesh::bsdf(int index)
{
    return _bsdfs[index];
}

Primitive *TriangleMesh::clone()
{
    return new TriangleMesh(*this);
}

}
