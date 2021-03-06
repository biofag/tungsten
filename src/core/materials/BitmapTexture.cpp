#include "BitmapTexture.hpp"

#include "primitives/IntersectionInfo.hpp"

#include "sampling/Distribution2D.hpp"

#include "math/MathUtil.hpp"
#include "math/Angle.hpp"

namespace Tungsten {

struct Rgba
{
    uint8 c[4];

    Vec3f normalize() const
    {
        return Vec3f(float(c[0]), float(c[1]), float(c[2]))*(1.0f/255.0f);
    }
};

BitmapTexture::BitmapTexture(const Path &path, TexelConversion conversion,
        bool gammaCorrect, bool linear, bool clamp)
: BitmapTexture(std::make_shared<Path>(path), conversion, gammaCorrect, linear, clamp)
{
}

BitmapTexture::BitmapTexture(PathPtr path, TexelConversion conversion,
        bool gammaCorrect, bool linear, bool clamp)
: _path(std::move(path)),
  _texelConversion(conversion),
  _gammaCorrect(gammaCorrect),
  _linear(linear),
  _clamp(clamp),
  _valid(false),
  _min(0.0f), _max(0.0f), _avg(0.0f),
  _texels(nullptr),
  _w(0), _h(0),
  _texelType(TexelType::SCALAR_LDR)
{
}

BitmapTexture::BitmapTexture(void *texels, int w, int h, TexelType texelType, bool linear, bool clamp)
: _linear(linear),
  _clamp(clamp),
  _valid(true)
{
    init(texels, w, h, texelType);
}

BitmapTexture::~BitmapTexture()
{
    switch (_texelType) {
    case TexelType::SCALAR_LDR: delete[] as<uint8>(); break;
    case TexelType::SCALAR_HDR: delete[] as<float>(); break;
    case TexelType::RGB_LDR: delete[] as<uint8>(); break;
    case TexelType::RGB_HDR: delete[] as<Vec3f>(); break;
    }
}

inline bool BitmapTexture::isRgb() const
{
    return (uint32(_texelType) & 2) != 0;
}

inline bool BitmapTexture::isHdr() const
{
    return (uint32(_texelType) & 1) != 0;
}

inline float BitmapTexture::lerp(float x00, float x01, float x10, float x11, float u, float v) const
{
    return (x00*(1.0f - u) + x01*u)*(1.0f - v) +
           (x10*(1.0f - u) + x11*u)*v;
}

inline Vec3f BitmapTexture::lerp(Vec3f x00, Vec3f x01, Vec3f x10, Vec3f x11, float u, float v) const
{
    return (x00*(1.0f - u) + x01*u)*(1.0f - v) +
           (x10*(1.0f - u) + x11*u)*v;
}

template<typename T>
inline const T *BitmapTexture::as() const
{
    return reinterpret_cast<const T *>(_texels);
}

inline float BitmapTexture::getScalar(int x, int y) const
{
    if (isHdr())
        return as<float>()[x + y*_w];
    else
        return float(as<uint8>()[x + y*_w])*(1.0f/255.0f);
}

inline Vec3f BitmapTexture::getRgb(int x, int y) const
{
    if (isHdr())
        return as<Vec3f>()[x + y*_w];
    else
        return as<Rgba>()[x + y*_w].normalize();
}

inline float BitmapTexture::weight(int x, int y) const
{
    if (isRgb())
        return getRgb(x, y).max();
    else
        return getScalar(x, y);
}

BitmapTexture::TexelType BitmapTexture::getTexelType(bool isRgb, bool isHdr)
{
    if (isRgb && isHdr)
        return TexelType::RGB_HDR;
    else if (isRgb)
        return TexelType::RGB_LDR;
    else if (isHdr)
        return TexelType::SCALAR_HDR;
    else
        return TexelType::SCALAR_LDR;
}

void BitmapTexture::init(void *texels, int w, int h, TexelType texelType)
{
    _texels = texels;
    _w = w;
    _h = h;
    _texelType = texelType;

    if (isRgb()) {
        _max = _min = getRgb(0, 0);
        _avg = Vec3f(0.0f);

        for (int y = 0; y < _h; ++y) {
            for (int x = 0; x < _w; ++x) {
                _min = min(_min, getRgb(x, y));
                _max = max(_max, getRgb(x, y));
                _avg += getRgb(x, y)/float(_w*_h);
            }
        }
    } else {
        float minT, maxT, avgT = 0.0f;
        minT = maxT = getScalar(0, 0);

        for (int y = 0; y < _h; ++y) {
            for (int x = 0; x < _w; ++x) {
                minT = min(minT, getScalar(x, y));
                maxT = max(maxT, getScalar(x, y));
                avgT += getScalar(x, y)/float(_w*_h);
            }
        }
        _min = Vec3f(minT);
        _max = Vec3f(maxT);
        _avg = Vec3f(avgT);
    }
}

void BitmapTexture::fromJson(const rapidjson::Value &/*v*/, const Scene &/*scene*/)
{
}

rapidjson::Value BitmapTexture::toJson(Allocator &allocator) const
{
    return std::move(rapidjson::Value(_path->asString().c_str(), allocator));
}

void BitmapTexture::loadResources()
{
    bool isRgb, isHdr;
    int w, h;
    void *pixels = nullptr;

    if (_path) {
        isRgb = _texelConversion == TexelConversion::REQUEST_RGB;
        isHdr = ImageIO::isHdr(*_path);

        if (isHdr)
            pixels = ImageIO::loadHdr(*_path, _texelConversion, w, h).release();
        else
            pixels = ImageIO::loadLdr(*_path, _texelConversion, w, h, _gammaCorrect).release();
    }

    if (!pixels) {
        uint8 *dummy = new uint8[4];
        dummy[0] = dummy[3] = 0xFF;
        dummy[1] = dummy[2] = 0;

        pixels = dummy;
        w = h = 2;
        isRgb = false;
        isHdr = false;

        if (_path)
            DBG("Unable to load texture at '%s'", *_path);
    } else {
        _valid = true;
    }

    init(pixels, w, h, getTexelType(isRgb, isHdr));
}

bool BitmapTexture::isConstant() const
{
    return false;
}

Vec3f BitmapTexture::average() const
{
    return Vec3f(_avg);
}

Vec3f BitmapTexture::minimum() const
{
    return Vec3f(_min);
}

Vec3f BitmapTexture::maximum() const
{
    return Vec3f(_max);
}

Vec3f BitmapTexture::operator[](const Vec2f &uv) const
{
    float u = uv.x()*_w;
    float v = (1.0f - uv.y())*_h;
    int iu = int(u);
    int iv = int(v);
    u -= iu;
    v -= iv;
    if (!_clamp) {
        iu = ((iu % _w) + _w) % _w;
        iv = ((iv % _h) + _h) % _h;
    }
    if (_linear) {
        iu = clamp(iu, 0, _w - 2);
        iv = clamp(iv, 0, _h - 2);
    } else {
        iu = clamp(iu, 0, _w - 1);
        iv = clamp(iv, 0, _h - 1);
    }

    if (!_linear) {
        if (isRgb())
            return getRgb(iu, iv);
        else
            return Vec3f(getScalar(iu, iv));
    }


    if (isRgb()) {
        return lerp(
            getRgb(iu, iv),
            getRgb(iu + 1, iv),
            getRgb(iu, iv + 1),
            getRgb(iu + 1, iv + 1),
            u,
            v
        );
    } else {
        return Vec3f(lerp(
            getScalar(iu, iv),
            getScalar(iu + 1, iv),
            getScalar(iu, iv + 1),
            getScalar(iu + 1, iv + 1),
            u,
            v
        ));
    }
}

Vec3f BitmapTexture::operator[](const IntersectionInfo &info) const
{
    return (*this)[info.uv];
}

void BitmapTexture::derivatives(const Vec2f &uv, Vec2f &derivs) const
{
    derivs = Vec2f(0.0f);
    float u = uv.x()*_w - 0.5f;
    float v = (1.0f - uv.y())*_h - 0.5f;
    int iu = int(u);
    int iv = int(v);
    u -= iu;
    v -= iv;
    iu = ((iu % _w) + _w) % _w;
    iv = ((iv % _h) + _h) % _h;
    int x0 = iu - 1, x1 = iu, x2 = (iu + 1) % _w, x3 = (iu + 2) % _w;
    int y0 = iv - 1, y1 = iv, y2 = (iv + 1) % _h, y3 = (iv + 2) % _h;
    if (x0 < 0) x0 = _w - 1;
    if (y0 < 0) y0 = _h - 1;

    // Filter footprint
    float      a01, a02;
    float a10, a11, a12, a13;
    float a20, a21, a22, a23;
    float      a31, a32;

    if (isRgb()) {
                                    a01 = getRgb(x1, y0).avg(), a02 = getRgb(x2, y0).avg();
        a10 = getRgb(x0, y1).avg(), a11 = getRgb(x1, y1).avg(), a12 = getRgb(x2, y1).avg(), a13 = getRgb(x3, y1).avg();
        a20 = getRgb(x0, y2).avg(), a21 = getRgb(x1, y2).avg(), a22 = getRgb(x2, y2).avg(), a23 = getRgb(x3, y2).avg();
                                    a31 = getRgb(x1, y3).avg(), a32 = getRgb(x2, y3).avg();
    } else {
                                 a01 = getScalar(x1, y0), a02 = getScalar(x2, y0);
        a10 = getScalar(x0, y1), a11 = getScalar(x1, y1), a12 = getScalar(x2, y1), a13 = getScalar(x3, y1);
        a20 = getScalar(x0, y2), a21 = getScalar(x1, y2), a22 = getScalar(x2, y2), a23 = getScalar(x3, y2);
                                 a31 = getScalar(x1, y3), a32 = getScalar(x2, y3);
    }

    float du11 = a12 - a10, du12 = a13 - a11, du21 = a22 - a20, du22 = a23 - a21;
    float dv11 = a21 - a01, dv21 = a31 - a11, dv12 = a22 - a02, dv22 = a32 - a12;

    derivs.x() = lerp(du11, du12, du21, du22, u, v)*float(_w);
    derivs.y() = lerp(dv11, dv12, dv21, dv22, u, v)*float(_h);
}

void BitmapTexture::makeSamplable(TextureMapJacobian jacobian)
{
    if (_distribution[jacobian])
        return;

    std::vector<float> weights(_w*_h);
    for (int y = 0, idx = 0; y < _h; ++y) {
        float rowWeight = 1.0f;
        if (jacobian == MAP_SPHERICAL)
            rowWeight *= std::sin((y*PI)/_h);
        for (int x = 0; x < _w; ++x, ++idx) {
            float w = weight(x, y)*4.0f
                    + weight((x + _w - 1) % _w, y)
                    + weight(x, (y + _h - 1) % _h)
                    + weight((x + 1) % _w, y)
                    + weight(x, (y + 1) % _h);

            weights[idx] = w*0.125f*rowWeight;
        }
    }
    _distribution[jacobian].reset(new Distribution2D(std::move(weights), _w, _h));
}

Vec2f BitmapTexture::sample(TextureMapJacobian jacobian, const Vec2f &uv) const
{
    Vec2f newUv(uv);
    int row, column;
    _distribution[jacobian]->warp(newUv, row, column);
    return Vec2f((newUv.x() + column)/_w, 1.0f - (newUv.y() + row)/_h);
}

float BitmapTexture::pdf(TextureMapJacobian jacobian, const Vec2f &uv) const
{
    return _distribution[jacobian]->pdf(int((1.0f - uv.y())*_h), int(uv.x()*_w))*_w*_h;
}

}
