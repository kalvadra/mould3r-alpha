// TetMeshJob.cpp — see TetMeshJob.h.
#include "TetMeshJob.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace TetMesh
{
    namespace
    {
        constexpr uint32_t kInMagic = 0x49334D33;   // "M3I" + version byte space
        constexpr uint32_t kOutMagic = 0x4F334D33;  // "M3O"
        constexpr uint32_t kVersion = 1;
        constexpr double   kPi = 3.14159265358979323846;

        template <class T> void Put(std::ofstream& f, const T& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
        template <class T> bool Get(std::ifstream& f, T& v) { return (bool)f.read(reinterpret_cast<char*>(&v), sizeof(T)); }

        template <class T> void PutVec(std::ofstream& f, const std::vector<T>& v)
        {
            const uint64_t n = v.size();
            Put(f, n);
            if (n) f.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)(n * sizeof(T)));
        }
        template <class T> bool GetVec(std::ifstream& f, std::vector<T>& v, uint64_t maxCount)
        {
            uint64_t n = 0;
            if (!Get(f, n) || n > maxCount) return false;
            v.resize((size_t)n);
            return n == 0 || (bool)f.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(n * sizeof(T)));
        }
        void PutString(std::ofstream& f, const std::string& s)
        {
            const uint32_t n = (uint32_t)std::min<size_t>(s.size(), 1u << 16);
            Put(f, n);
            f.write(s.data(), n);
        }
        bool GetString(std::ifstream& f, std::string& s)
        {
            uint32_t n = 0;
            if (!Get(f, n) || n > (1u << 16)) return false;
            s.resize(n);
            return n == 0 || (bool)f.read(s.data(), n);
        }

        constexpr uint64_t kMaxElems = uint64_t(1) << 34;   // sanity cap for corrupt files

        struct Vec3 { double x, y, z; };
        Vec3 Sub(const double* a, const double* b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
        Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        double Len(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    }

    // ---------------------------------------------------------------------
    Surface WeldSurface(const float* data, size_t vertexCount, size_t stride,
                        const uint32_t* indices, size_t indexCount)
    {
        Surface s;
        if (!data || !indices || stride < 3) return s;

        struct Key
        {
            uint32_t b[3];
            bool operator==(const Key& o) const { return b[0] == o.b[0] && b[1] == o.b[1] && b[2] == o.b[2]; }
        };
        struct KeyHash
        {
            size_t operator()(const Key& k) const
            {
                uint64_t h = 1469598103934665603ull;
                for (uint32_t v : k.b) { h ^= v; h *= 1099511628211ull; }
                return (size_t)h;
            }
        };
        std::unordered_map<Key, int32_t, KeyHash> map;
        map.reserve(vertexCount);
        std::vector<int32_t> remap(vertexCount, -1);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            const float* p = data + i * stride;
            float q[3] = { p[0], p[1], p[2] };
            for (float& c : q) if (c == 0.0f) c = 0.0f;   // -0 and +0 weld together
            Key k;
            std::memcpy(k.b, q, sizeof(k.b));
            auto it = map.find(k);
            if (it == map.end())
            {
                const int32_t id = (int32_t)(s.verts.size() / 3);
                map.emplace(k, id);
                s.verts.insert(s.verts.end(), { (double)q[0], (double)q[1], (double)q[2] });
                remap[i] = id;
            }
            else
                remap[i] = it->second;
        }
        s.tris.reserve(indexCount);
        for (size_t t = 0; t + 2 < indexCount; t += 3)
        {
            if (indices[t] >= vertexCount || indices[t + 1] >= vertexCount || indices[t + 2] >= vertexCount) continue;
            const int32_t a = remap[indices[t]], b = remap[indices[t + 1]], c = remap[indices[t + 2]];
            if (a == b || b == c || a == c) continue;   // collapsed by the weld
            s.tris.insert(s.tris.end(), { a, b, c });
        }
        return s;
    }

    double SurfaceVolume(const Surface& s)
    {
        double v = 0.0;
        for (size_t t = 0; t + 2 < s.tris.size(); t += 3)
        {
            const double* a = &s.verts[3 * (size_t)s.tris[t]];
            const double* b = &s.verts[3 * (size_t)s.tris[t + 1]];
            const double* c = &s.verts[3 * (size_t)s.tris[t + 2]];
            v += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                 a[2] * (b[0] * c[1] - b[1] * c[0]);
        }
        return v / 6.0;
    }

    // ---------------------------------------------------------------------
    Stats ComputeStats(const Mesh& m)
    {
        Stats st;
        st.tets = m.TetCount();
        st.verts = m.VertexCount();
        if (st.tets == 0) return st;

        st.minDihedralDeg = 180.0;
        st.maxDihedralDeg = 0.0;
        double edgeSum = 0.0;
        static const int kEdge[6][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 } };

        struct FaceKey
        {
            int32_t v[3];
            bool operator==(const FaceKey& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2]; }
        };
        struct FaceHash
        {
            size_t operator()(const FaceKey& k) const
            {
                uint64_t h = 1469598103934665603ull;
                for (int32_t x : k.v) { h ^= (uint32_t)x; h *= 1099511628211ull; }
                return (size_t)h;
            }
        };
        std::unordered_map<FaceKey, int32_t, FaceHash> faces;
        faces.reserve(st.tets * 2);

        for (size_t i = 0; i < st.tets; ++i)
        {
            const int32_t* t = &m.tets[4 * i];
            const double* p[4] = { &m.verts[3 * (size_t)t[0]], &m.verts[3 * (size_t)t[1]],
                                   &m.verts[3 * (size_t)t[2]], &m.verts[3 * (size_t)t[3]] };
            const double vol = Dot(Sub(p[1], p[0]), Cross(Sub(p[2], p[0]), Sub(p[3], p[0]))) / 6.0;
            st.volumeMm3 += vol;
            if (vol <= 0.0) ++st.nonPositive;

            for (const auto& e : kEdge) edgeSum += Len(Sub(p[e[1]], p[e[0]]));

            // Outward unit normals; the dihedral between faces f and g is
            // pi - angle(n_f, n_g).
            Vec3 n[4];
            for (int f = 0; f < 4; ++f)
            {
                const Vec3 c = Cross(Sub(p[kTetFace[f][1]], p[kTetFace[f][0]]), Sub(p[kTetFace[f][2]], p[kTetFace[f][0]]));
                const double l = Len(c);
                n[f] = l > 0.0 ? Vec3{ c.x / l, c.y / l, c.z / l } : Vec3{ 0, 0, 0 };
            }
            double tmin = 180.0;
            for (int f = 0; f < 4; ++f)
                for (int g = f + 1; g < 4; ++g)
                {
                    const double cosang = std::clamp(-Dot(n[f], n[g]), -1.0, 1.0);
                    const double deg = std::acos(cosang) * 180.0 / kPi;
                    tmin = std::min(tmin, deg);
                    st.minDihedralDeg = std::min(st.minDihedralDeg, deg);
                    st.maxDihedralDeg = std::max(st.maxDihedralDeg, deg);
                }
            if (tmin < 5.0) ++st.slivers;

            for (const auto& f : kTetFace)
            {
                FaceKey k{ { t[f[0]], t[f[1]], t[f[2]] } };
                std::sort(k.v, k.v + 3);
                ++faces[k];
            }
        }
        for (const auto& kv : faces)
            if (kv.second == 1) ++st.boundaryFaces;
        st.meanEdgeMm = edgeSum / (6.0 * (double)st.tets);
        return st;
    }

    // ---------------------------------------------------------------------
    bool WriteJobInput(const fs::path& path, const Surface& s, const Params& p, std::string& error)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { error = "Could not create the mesh job file."; return false; }
        Put(f, kInMagic);
        Put(f, kVersion);
        Put(f, p.edgeLength);
        Put(f, p.epsilon);
        Put(f, p.stopEnergy);
        Put(f, p.maxPasses);
        Put(f, p.maxThreads);
        Put(f, p.coarsen);
        PutVec(f, s.verts);
        PutVec(f, s.tris);
        f.close();
        if (!f) { error = "Could not write the mesh job file (disk full?)."; return false; }
        return true;
    }

    bool ReadJobInput(const fs::path& path, Surface& s, Params& p, std::string& error)
    {
        std::ifstream f(path, std::ios::binary);
        uint32_t magic = 0, version = 0;
        if (!f || !Get(f, magic) || magic != kInMagic || !Get(f, version) || version != kVersion)
        {
            error = "Not a Mould3r mesh job file (or a different version).";
            return false;
        }
        const bool ok = Get(f, p.edgeLength) && Get(f, p.epsilon) && Get(f, p.stopEnergy) &&
                        Get(f, p.maxPasses) && Get(f, p.maxThreads) && Get(f, p.coarsen) &&
                        GetVec(f, s.verts, kMaxElems) && GetVec(f, s.tris, kMaxElems) &&
                        s.verts.size() % 3 == 0 && s.tris.size() % 3 == 0;
        if (!ok) { error = "The mesh job file is truncated or corrupt."; return false; }
        return true;
    }

    bool WriteJobOutput(const fs::path& path, const JobResult& r, std::string& error)
    {
        fs::path part = path;
        part += ".part";
        {
            std::ofstream f(part, std::ios::binary | std::ios::trunc);
            if (!f) { error = "Could not create the mesh result file."; return false; }
            Put(f, kOutMagic);
            Put(f, kVersion);
            Put(f, r.status);
            Put(f, r.workerSeconds);
            PutString(f, r.message);
            PutVec(f, r.mesh.verts);
            PutVec(f, r.mesh.tets);
            f.close();
            if (!f) { error = "Could not write the mesh result file (disk full?)."; return false; }
        }
        std::error_code ec;
        fs::rename(part, path, ec);   // replaces an existing file (MoveFileEx REPLACE_EXISTING on Windows)
        if (ec) { error = "Could not finalise the mesh result file: " + ec.message(); return false; }
        return true;
    }

    bool ReadJobOutput(const fs::path& path, JobResult& r, std::string& error)
    {
        r = JobResult{};
        std::ifstream f(path, std::ios::binary);
        uint32_t magic = 0, version = 0;
        if (!f || !Get(f, magic) || magic != kOutMagic || !Get(f, version) || version != kVersion)
        {
            error = "The mesh result file is missing or from a different version.";
            return false;
        }
        const bool ok = Get(f, r.status) && Get(f, r.workerSeconds) && GetString(f, r.message) &&
                        GetVec(f, r.mesh.verts, kMaxElems) && GetVec(f, r.mesh.tets, kMaxElems) &&
                        r.mesh.verts.size() % 3 == 0 && r.mesh.tets.size() % 4 == 0;
        if (!ok) { error = "The mesh result file is truncated or corrupt."; return false; }
        // Guard every index before anyone dereferences it.
        const int32_t nv = (int32_t)r.mesh.VertexCount();
        for (int32_t v : r.mesh.tets)
            if (v < 0 || v >= nv) { error = "The mesh result references a vertex out of range."; return false; }
        return true;
    }
    // ---------------------------------------------------------------------
    std::vector<float> TetMinDihedral(const Mesh& m)
    {
        const size_t nt = m.TetCount();
        std::vector<float> q(nt, 0.0f);
        for (size_t i = 0; i < nt; ++i)
        {
            const int32_t* t = &m.tets[4 * i];
            const double* p[4] = { &m.verts[3 * (size_t)t[0]], &m.verts[3 * (size_t)t[1]],
                                   &m.verts[3 * (size_t)t[2]], &m.verts[3 * (size_t)t[3]] };
            Vec3 n[4];
            bool degenerate = false;
            for (int f = 0; f < 4; ++f)
            {
                const Vec3 c = Cross(Sub(p[kTetFace[f][1]], p[kTetFace[f][0]]), Sub(p[kTetFace[f][2]], p[kTetFace[f][0]]));
                const double l = Len(c);
                if (!(l > 0.0)) { degenerate = true; break; }
                n[f] = Vec3{ c.x / l, c.y / l, c.z / l };
            }
            if (degenerate) continue;   // 0 degrees
            double tmin = 180.0;
            for (int f = 0; f < 4; ++f)
                for (int g = f + 1; g < 4; ++g)
                    tmin = std::min(tmin, std::acos(std::clamp(-Dot(n[f], n[g]), -1.0, 1.0)) * 180.0 / kPi);
            q[i] = (float)tmin;
        }
        return q;
    }

    std::vector<int32_t> TetFaceNeighbours(const Mesh& m)
    {
        const size_t nt = m.TetCount();
        std::vector<int32_t> nb(4 * nt, -1);
        // Sort every face by its (sorted) vertex triple; interior faces come
        // out as adjacent equal pairs. Cheaper than a hash map at 10^6 faces.
        struct F { int32_t a, b, c; uint32_t slot; };
        std::vector<F> faces;
        faces.reserve(4 * nt);
        for (size_t i = 0; i < nt; ++i)
        {
            const int32_t* t = &m.tets[4 * i];
            for (int f = 0; f < 4; ++f)
            {
                int32_t v[3] = { t[kTetFace[f][0]], t[kTetFace[f][1]], t[kTetFace[f][2]] };
                std::sort(v, v + 3);
                faces.push_back({ v[0], v[1], v[2], (uint32_t)(4 * i + (size_t)f) });
            }
        }
        std::sort(faces.begin(), faces.end(), [](const F& x, const F& y)
        {
            if (x.a != y.a) return x.a < y.a;
            if (x.b != y.b) return x.b < y.b;
            return x.c < y.c;
        });
        for (size_t k = 0; k + 1 < faces.size();)
        {
            const F& x = faces[k];
            const F& y = faces[k + 1];
            if (x.a == y.a && x.b == y.b && x.c == y.c)
            {
                nb[x.slot] = (int32_t)(y.slot / 4);
                nb[y.slot] = (int32_t)(x.slot / 4);
                k += 2;
            }
            else
                ++k;
        }
        return nb;
    }

    // ---------------------------------------------------------------------
    namespace
    {
        // The boundary triangles of a tagged mesh (outward winding), in slot order.
        struct TaggedTri { int32_t v[3]; uint8_t tag; };
        std::vector<TaggedTri> TaggedBoundary(const Mesh& m, const std::vector<uint8_t>* slotTag)
        {
            std::vector<TaggedTri> out;
            if (!slotTag || slotTag->size() != 4 * m.TetCount()) return out;
            for (size_t s = 0; s < slotTag->size(); ++s)
            {
                const uint8_t tag = (*slotTag)[s];
                if (tag == 255) continue;
                const int32_t* t = &m.tets[4 * (s / 4)];
                const int f = (int)(s % 4);
                out.push_back({ { t[kTetFace[f][0]], t[kTetFace[f][1]], t[kTetFace[f][2]] }, tag });
            }
            return out;
        }
    }

    bool WriteVtu(const fs::path& path, const Mesh& m, const std::vector<float>* minDihedral, std::string& error,
                  const std::vector<uint8_t>* slotTag,
                  const std::vector<std::pair<std::string, const std::vector<float>*>>& pointData)
    {
        const size_t nv = m.VertexCount(), nt = m.TetCount();
        if (nt == 0) { error = "There is no mesh to export."; return false; }
        const bool withQ = minDihedral && minDihedral->size() == nt;
        const std::vector<TaggedTri> tris = TaggedBoundary(m, slotTag);
        const bool withB = !tris.empty();
        const size_t nc = nt + tris.size();

        // Cell arrays: tets first, then the boundary triangles.
        std::vector<int32_t> con;
        con.reserve(4 * nt + 3 * tris.size());
        con.insert(con.end(), m.tets.begin(), m.tets.end());
        for (const TaggedTri& t : tris) con.insert(con.end(), { t.v[0], t.v[1], t.v[2] });
        std::vector<int32_t> off(nc);
        std::vector<uint8_t> types(nc);
        int32_t o = 0;
        for (size_t i = 0; i < nc; ++i)
        {
            const bool tet = i < nt;
            o += tet ? 4 : 3;
            off[i] = o;
            types[i] = tet ? (uint8_t)10 : (uint8_t)5;   // VTK_TETRA / VTK_TRIANGLE
        }
        std::vector<float> q;
        if (withQ)
        {
            q.assign(minDihedral->begin(), minDihedral->end());
            q.resize(nc, -1.0f);
        }
        std::vector<int32_t> btag;
        if (withB)
        {
            btag.assign(nc, -1);
            for (size_t i = 0; i < tris.size(); ++i) btag[nt + i] = (int32_t)tris[i].tag;
        }

        // Appended raw binary blocks, each preceded by its byte count (UInt64
        // header). Offsets count from the first byte after the '_' marker.
        const uint64_t bPts = (uint64_t)(3 * nv * sizeof(double));
        const uint64_t bCon = (uint64_t)(con.size() * sizeof(int32_t));
        const uint64_t bOff = (uint64_t)(nc * sizeof(int32_t));
        const uint64_t bTyp = (uint64_t)(nc * sizeof(uint8_t));
        const uint64_t bQ = withQ ? (uint64_t)(nc * sizeof(float)) : 0;
        const uint64_t bB = withB ? (uint64_t)(nc * sizeof(int32_t)) : 0;
        const uint64_t oPts = 0;
        const uint64_t oCon = oPts + 8 + bPts;
        const uint64_t oOff = oCon + 8 + bCon;
        const uint64_t oTyp = oOff + 8 + bOff;
        const uint64_t oQ = oTyp + 8 + bTyp;
        const uint64_t oB = oQ + (withQ ? 8 + bQ : 0);
        // Point data blocks follow the cell data.
        std::vector<std::pair<std::string, const std::vector<float>*>> pd;
        for (const auto& a : pointData)
            if (a.second && a.second->size() == nv) pd.push_back(a);
        const uint64_t bPd = (uint64_t)(nv * sizeof(float));
        const uint64_t oPd0 = oB + (withB ? 8 + bB : 0);

        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { error = "Could not create the file."; return false; }
        const uint16_t probe = 1;
        const bool little = *reinterpret_cast<const uint8_t*>(&probe) == 1;
        f << "<?xml version=\"1.0\"?>\n"
          << "<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" byte_order=\""
          << (little ? "LittleEndian" : "BigEndian") << "\" header_type=\"UInt64\">\n"
          << "  <UnstructuredGrid>\n"
          << "    <Piece NumberOfPoints=\"" << nv << "\" NumberOfCells=\"" << nc << "\">\n"
          << "      <Points>\n"
          << "        <DataArray type=\"Float64\" NumberOfComponents=\"3\" format=\"appended\" offset=\"" << oPts << "\"/>\n"
          << "      </Points>\n"
          << "      <Cells>\n"
          << "        <DataArray type=\"Int32\" Name=\"connectivity\" format=\"appended\" offset=\"" << oCon << "\"/>\n"
          << "        <DataArray type=\"Int32\" Name=\"offsets\" format=\"appended\" offset=\"" << oOff << "\"/>\n"
          << "        <DataArray type=\"UInt8\" Name=\"types\" format=\"appended\" offset=\"" << oTyp << "\"/>\n"
          << "      </Cells>\n";
        if (withQ || withB)
        {
            f << "      <CellData" << (withQ ? " Scalars=\"min_dihedral_deg\"" : "") << ">\n";
            if (withQ)
                f << "        <DataArray type=\"Float32\" Name=\"min_dihedral_deg\" format=\"appended\" offset=\"" << oQ << "\"/>\n";
            if (withB)
                f << "        <DataArray type=\"Int32\" Name=\"boundary\" format=\"appended\" offset=\"" << oB << "\"/>\n";
            f << "      </CellData>\n";
        }
        if (!pd.empty())
        {
            f << "      <PointData>\n";
            for (size_t k = 0; k < pd.size(); ++k)
                f << "        <DataArray type=\"Float32\" Name=\"" << pd[k].first << "\" format=\"appended\" offset=\""
                  << (oPd0 + k * (8 + bPd)) << "\"/>\n";
            f << "      </PointData>\n";
        }
        f << "    </Piece>\n"
          << "  </UnstructuredGrid>\n"
          << "  <AppendedData encoding=\"raw\">\n   _";

        auto block = [&f](const void* data, uint64_t bytes)
        {
            Put(f, bytes);
            if (bytes) f.write(reinterpret_cast<const char*>(data), (std::streamsize)bytes);
        };
        block(m.verts.data(), bPts);
        block(con.data(), bCon);
        block(off.data(), bOff);
        block(types.data(), bTyp);
        if (withQ) block(q.data(), bQ);
        if (withB) block(btag.data(), bB);
        for (const auto& a : pd) block(a.second->data(), bPd);
        f << "\n  </AppendedData>\n</VTKFile>\n";
        f.close();
        if (!f) { error = "Could not write the file (disk full?)."; return false; }
        return true;
    }

    bool WriteGmshMsh(const fs::path& path, const Mesh& m, std::string& error,
                      const std::vector<uint8_t>* slotTag, const std::vector<std::string>& tagNames)
    {
        const size_t nv = m.VertexCount(), nt = m.TetCount();
        if (nt == 0) { error = "There is no mesh to export."; return false; }
        const std::vector<TaggedTri> tris = TaggedBoundary(m, slotTag);
        constexpr int kVolumeGroup = 100;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);   // binary: '\n' line ends on every OS
        if (!f) { error = "Could not create the file."; return false; }
        f.precision(17);
        f << "$MeshFormat\n2.2 0 8\n$EndMeshFormat\n";
        if (!tris.empty())
        {
            // Physical names: dimension, tag, "name".
            bool used[256] = {};
            for (const TaggedTri& t : tris) used[t.tag] = true;
            std::vector<std::pair<int, std::string>> names;
            for (int tg = 0; tg < 255; ++tg)
                if (used[tg])
                    names.push_back({ tg + 1, (size_t)tg < tagNames.size() ? tagNames[(size_t)tg]
                                                                           : "boundary_" + std::to_string(tg) });
            f << "$PhysicalNames\n" << (names.size() + 1) << "\n";
            for (const auto& [tg, nm] : names) f << "2 " << tg << " \"" << nm << "\"\n";
            f << "3 " << kVolumeGroup << " \"volume\"\n$EndPhysicalNames\n";
        }
        f << "$Nodes\n" << nv << "\n";
        for (size_t i = 0; i < nv; ++i)
            f << (i + 1) << ' ' << m.verts[3 * i] << ' ' << m.verts[3 * i + 1] << ' ' << m.verts[3 * i + 2] << '\n';
        f << "$EndNodes\n$Elements\n" << (nt + tris.size()) << "\n";
        size_t id = 1;
        // Boundary triangles first (type 2), then the tets (type 4); 2 tags =
        // physical group, elementary entity.
        for (const TaggedTri& t : tris)
            f << id++ << " 2 2 " << (t.tag + 1) << ' ' << (t.tag + 1) << ' '
              << (t.v[0] + 1) << ' ' << (t.v[1] + 1) << ' ' << (t.v[2] + 1) << '\n';
        const int vg = tris.empty() ? 1 : kVolumeGroup;
        for (size_t i = 0; i < nt; ++i)
        {
            const int32_t* t = &m.tets[4 * i];
            f << id++ << " 4 2 " << vg << ' ' << vg << ' '
              << (t[0] + 1) << ' ' << (t[1] + 1) << ' ' << (t[2] + 1) << ' ' << (t[3] + 1) << '\n';
        }
        f << "$EndElements\n";
        f.close();
        if (!f) { error = "Could not write the file (disk full?)."; return false; }
        return true;
    }
}
