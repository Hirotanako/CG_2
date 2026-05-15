#include "ObjLoader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <DirectXMath.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace Obj
{
namespace
{

static void TrimInPlace(std::string& s)
{
    while (!s.empty() && (static_cast<unsigned char>(s.front()) <= ' '))
        s.erase(s.begin());
    while (!s.empty() && (static_cast<unsigned char>(s.back()) <= ' '))
        s.pop_back();
}

static std::wstring Utf8ToWide(std::string_view sv)
{
    if (sv.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), n);
    return out;
}

static bool ReadLine(std::ifstream& f, std::string& line)
{
    if (!std::getline(f, line))
        return false;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return true;
}

static bool ParseFloat3(const std::string& rest, float out[3])
{
    std::istringstream iss(rest);
    iss >> out[0] >> out[1] >> out[2];
    return !iss.fail();
}

static bool ParseFloat2(const std::string& rest, float out[2])
{
    std::istringstream iss(rest);
    iss >> out[0] >> out[1];
    return !iss.fail();
}

struct VKey
{
    int32_t vi = 0;
    int32_t vti = 0;
    int32_t vni = 0;
    bool operator==(const VKey& o) const { return vi == o.vi && vti == o.vti && vni == o.vni; }
};

struct VKeyHash
{
    size_t operator()(const VKey& k) const noexcept
    {
        const uint64_t a = static_cast<uint32_t>(k.vi);
        const uint64_t b = static_cast<uint32_t>(k.vti);
        const uint64_t c = static_cast<uint32_t>(k.vni);
        return static_cast<size_t>(a ^ (b << 1) ^ (c << 2) ^ (a >> 3));
    }
};

static size_t ResolveIndex(int idx, size_t count)
{
    if (idx > 0)
        return static_cast<size_t>(idx - 1);
    if (idx < 0)
        return count + static_cast<size_t>(idx);
    return 0;
}

static bool LoadMtl(const std::filesystem::path& mtlPath, std::vector<Material>& outMaterials, std::wstring& err)
{
    std::ifstream f(mtlPath);
    if (!f)
    {
        err = L"Не удалось открыть MTL: " + mtlPath.wstring();
        return false;
    }

    outMaterials.clear();
    Material* cur = nullptr;
    std::string line;
    while (ReadLine(f, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t sp = line.find_first_of(" \t");
        std::string tag = sp == std::string::npos ? line : line.substr(0, sp);
        std::string rest = sp == std::string::npos ? std::string{} : line.substr(sp + 1);
        TrimInPlace(rest);

        if (tag == "newmtl")
        {
            outMaterials.push_back({});
            cur = &outMaterials.back();
            cur->name = rest;
            TrimInPlace(cur->name);
            continue;
        }
        if (!cur)
            continue;

        if (tag == "Kd")
        {
            float c[3]{};
            if (ParseFloat3(rest, c))
            {
                cur->Kd[0] = c[0];
                cur->Kd[1] = c[1];
                cur->Kd[2] = c[2];
            }
        }
        else if (tag == "map_Kd")
        {
            std::wstring w = Utf8ToWide(rest);
            if (!w.empty() && w.front() == L'"' && w.back() == L'"' && w.size() >= 2)
                w = w.substr(1, w.size() - 2);
            for (wchar_t& ch : w)
            {
                if (ch == L'/')
                    ch = L'\\';
            }
            cur->diffuseMapRel = std::move(w);
        }
    }
    return true;
}

static void SplitFaceToken(std::string_view tok, int32_t& vi, int32_t& vti, int32_t& vni)
{
    vi = vti = vni = 0;
    size_t p = 0;
    auto next = [&](int32_t& out) {
        out = 0;
        if (p >= tok.size())
            return;
        size_t e = tok.find('/', p);
        std::string_view part = (e == std::string_view::npos) ? tok.substr(p) : tok.substr(p, e - p);
        p = (e == std::string_view::npos) ? tok.size() : e + 1;
        if (part.empty())
            return;
        int v = 0;
        auto r = std::from_chars(part.data(), part.data() + part.size(), v);
        if (r.ec == std::errc())
            out = static_cast<int32_t>(v);
    };
    next(vi);
    next(vti);
    next(vni);
}

} // namespace

bool LoadObj(const std::filesystem::path& objPath, LoadedMesh& out, std::wstring& error)
{
    out = {};
    error.clear();

    std::ifstream file(objPath);
    if (!file)
    {
        error = L"Не удалось открыть OBJ: " + objPath.wstring();
        return false;
    }

    const std::filesystem::path objDir = objPath.parent_path();

    std::vector<std::string> lines;
    std::string ln;
    while (ReadLine(file, ln))
        lines.push_back(std::move(ln));
    file.close();

    for (const std::string& line : lines)
    {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t sp = line.find_first_of(" \t");
        std::string tag = sp == std::string::npos ? line : line.substr(0, sp);
        if (tag != "mtllib")
            continue;
        std::string rest = sp == std::string::npos ? std::string{} : line.substr(sp + 1);
        TrimInPlace(rest);
        if (rest.empty())
            continue;
        const std::filesystem::path mtlPath = objDir / rest;
        if (!LoadMtl(mtlPath, out.materials, error))
            return false;
        break;
    }

    std::unordered_map<std::string, uint32_t> matByName;
    for (uint32_t i = 0; i < out.materials.size(); ++i)
        matByName[out.materials[i].name] = i;

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> uvs;

    std::unordered_map<VKey, uint32_t, VKeyHash> vertCache;
    vertCache.reserve(131072);

    std::vector<uint32_t> curIndices;
    uint32_t curMat = 0;

    auto flushGroup = [&]() {
        if (curIndices.empty())
            return;
        Submesh sm{};
        sm.indexStart = static_cast<uint32_t>(out.indices.size());
        sm.indexCount = static_cast<uint32_t>(curIndices.size());
        sm.materialIndex = curMat;
        out.indices.insert(out.indices.end(), curIndices.begin(), curIndices.end());
        out.submeshes.push_back(sm);
        curIndices.clear();
    };

    auto addCorner = [&](int32_t vi, int32_t vti, int32_t vni) -> uint32_t {
        VKey key{vi, vti, vni};
        auto it = vertCache.find(key);
        if (it != vertCache.end())
            return it->second;

        const size_t pi = ResolveIndex(vi, positions.size());
        const size_t ni = (vni != 0) ? ResolveIndex(vni, normals.size()) : 0;
        const size_t ti = (vti != 0) ? ResolveIndex(vti, uvs.size()) : 0;

        MeshVertex mv{};
        if (pi < positions.size())
        {
            mv.px = positions[pi].x;
            mv.py = positions[pi].y;
            mv.pz = positions[pi].z;
        }
        if (ni < normals.size())
        {
            mv.nx = normals[ni].x;
            mv.ny = normals[ni].y;
            mv.nz = normals[ni].z;
        }
        else
        {
            mv.nx = 0;
            mv.ny = 1;
            mv.nz = 0;
        }
        if (ti < uvs.size())
        {
            mv.u = uvs[ti].x;
            mv.v = uvs[ti].y;
        }

        const uint32_t idx = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(mv);
        vertCache.emplace(key, idx);
        return idx;
    };

    for (const std::string& line : lines)
    {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t sp = line.find_first_of(" \t");
        std::string tag = sp == std::string::npos ? line : line.substr(0, sp);
        std::string rest = sp == std::string::npos ? std::string{} : line.substr(sp + 1);
        TrimInPlace(rest);

        if (tag == "mtllib")
            continue;
        if (tag == "v")
        {
            float p[3]{};
            if (ParseFloat3(rest, p))
                positions.push_back(XMFLOAT3{p[0], p[1], p[2]});
            continue;
        }
        if (tag == "vn")
        {
            float n[3]{};
            if (ParseFloat3(rest, n))
            {
                XMVECTOR vn = XMVector3Normalize(XMVectorSet(n[0], n[1], n[2], 0));
                XMFLOAT3 fn{};
                XMStoreFloat3(&fn, vn);
                normals.push_back(fn);
            }
            continue;
        }
        if (tag == "vt")
        {
            float t[2]{};
            if (ParseFloat2(rest, t))
                uvs.push_back(XMFLOAT2{t[0], t[1]});
            continue;
        }
        if (tag == "usemtl")
        {
            flushGroup();
            TrimInPlace(rest);
            auto it = matByName.find(rest);
            curMat = (it != matByName.end()) ? it->second : 0;
            continue;
        }
        if (tag == "f")
        {
            std::istringstream iss(rest);
            std::vector<std::string> corners;
            std::string c;
            while (iss >> c)
                corners.push_back(c);
            if (corners.size() < 3)
                continue;

            auto emitTri = [&](const std::string& a, const std::string& b, const std::string& c2) {
                int32_t vi, vti, vni;
                SplitFaceToken(a, vi, vti, vni);
                const uint32_t ia = addCorner(vi, vti, vni);
                SplitFaceToken(b, vi, vti, vni);
                const uint32_t ib = addCorner(vi, vti, vni);
                SplitFaceToken(c2, vi, vti, vni);
                const uint32_t ic = addCorner(vi, vti, vni);
                curIndices.push_back(ia);
                curIndices.push_back(ib);
                curIndices.push_back(ic);
            };

            if (corners.size() == 3)
            {
                emitTri(corners[0], corners[1], corners[2]);
            }
            else if (corners.size() == 4)
            {
                emitTri(corners[0], corners[1], corners[2]);
                emitTri(corners[0], corners[2], corners[3]);
            }
            else
            {
                for (size_t i = 1; i + 1 < corners.size(); ++i)
                    emitTri(corners[0], corners[i], corners[i + 1]);
            }
            continue;
        }
    }

    flushGroup();

    if (out.materials.empty())
    {
        out.materials.push_back({});
        out.materials.back().name = "default";
    }

    return !out.vertices.empty() && !out.indices.empty();
}

} // namespace Obj
