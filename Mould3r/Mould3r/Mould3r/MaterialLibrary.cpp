#include "MaterialLibrary.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "TestMaterial.h"

namespace fs = std::filesystem;

namespace
{
    // A path component as UTF-8 (display names are UTF-8 throughout; a plain
    // .string() is the ANSI code page on Windows).
    std::string U8(const fs::path& p)
    {
        const auto u = p.u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }

    std::string Lower(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    }

    // Scan one library folder into `out` (appended after the built-ins
    // already in it). Display names are made unique case-insensitively by
    // appending the file name.
    template <class Data>
    void ScanFolder(MaterialKind kind, std::vector<MaterialLibraryEntry<Data>>& out,
                    std::vector<std::string>& problems)
    {
        const std::string folder = MaterialFile::LibraryFolder(kind);
        std::vector<MaterialLibraryEntry<Data>> found;

        std::error_code ec;
        fs::directory_iterator it(fs::path(folder), ec), end;
        if (ec)
        {
            problems.push_back("Could not read " + folder + ": " + ec.message());
            return;
        }
        for (; it != end; it.increment(ec))
        {
            if (ec) { problems.push_back("Error scanning " + folder + ": " + ec.message()); break; }
            try
            {
                std::error_code fec;
                if (!it->is_regular_file(fec)) continue;
                const fs::path p = it->path();
                if (Lower(p.extension().string()) != kMaterialFileExtension) continue;

                MaterialLibraryEntry<Data> e;
                e.path = p.string();
                std::string err;
                if (!MaterialFile::Load(e.path, e.data, err, nullptr))
                {
                    problems.push_back(err);
                    continue;
                }
                e.displayName = e.data.name.empty() ? U8(p.stem()) : e.data.name;
                found.push_back(std::move(e));
            }
            catch (const std::exception& ex)
            {
                // e.g. a file name that can't be represented in the ANSI code page.
                problems.push_back(std::string("Skipped a file in ") + folder + ": " + ex.what());
            }
        }

        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b)
        {
            const std::string la = Lower(a.displayName), lb = Lower(b.displayName);
            return la != lb ? la < lb : a.path < b.path;
        });

        for (auto& e : found)
        {
            auto taken = [&](const std::string& name)
            {
                for (const auto& x : out) if (Lower(x.displayName) == Lower(name)) return true;
                return false;
            };
            if (taken(e.displayName))
            {
                const std::string base = e.displayName + " (" + U8(fs::path(e.path).filename()) + ")";
                std::string name = base;
                for (int n = 2; taken(name); ++n) name = base + " " + std::to_string(n);
                e.displayName = name;
            }
            out.push_back(std::move(e));
        }
    }

    template <class Data>
    int FindKey(const std::vector<MaterialLibraryEntry<Data>>& v, const std::string& key)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].Key() == key) return (int)i;
        return -1;
    }

    template <class Data>
    bool FetchImpl(const MaterialLibraryEntry<Data>& e, Data& out, std::string& error,
                   std::vector<std::string>* warnings)
    {
        if (e.builtIn) { out = e.data; return true; }
        return MaterialFile::Load(e.path, out, error, warnings);
    }
} // namespace

// ---------------------------------------------------------------------------
// Built-ins — the TestMaterial figures in library form.
// ---------------------------------------------------------------------------
InjectionMaterialData BuiltInPolypropylene()
{
    const TestMaterial::PolymerMaterial& p = TestMaterial::kGenericPolypropylene;
    InjectionMaterialData d;
    d.name = p.name;
    d.family = "PP";
    d.structure = "semi-crystalline";
    d.filler = "none";
    d.notes = "Mould3r built-in: representative generic values, not certified datasheet data";
    d.density = p.densitySolid / 1000.0;
    d.meltTempMin = p.meltTempMinC;
    d.meltTempMax = p.meltTempMaxC;
    d.meltTemp = p.recMeltTempC;
    d.mouldTemp = p.recMouldTempC;
    d.ejectionTemp = p.ejectionTempC;
    d.maxShearRate = p.maxShearRate;
    d.tensileModulus = p.elasticModulusMPa;
    d.poissonRatio = p.poissonRatio;
    d.meltDensity = p.densityMelt / 1000.0;
    d.meltSpecificHeat = p.specificHeat;
    d.meltThermalConductivity = p.thermalConductivity;
    d.noFlowTemp = p.noFlowTempC;
    const TestMaterial::CrossWLF& c = p.viscosity;
    d.cwlfN = c.n; d.cwlfTauStar = c.tauStar; d.cwlfD1 = c.D1; d.cwlfD2 = c.D2;
    d.cwlfD3 = c.D3; d.cwlfA1 = c.A1; d.cwlfA2 = c.A2tilde;
    const TestMaterial::TaitPVT& t = p.pvt;
    d.taitB1m = t.b1m; d.taitB2m = t.b2m; d.taitB3m = t.b3m; d.taitB4m = t.b4m;
    d.taitB1s = t.b1s; d.taitB2s = t.b2s; d.taitB3s = t.b3s; d.taitB4s = t.b4s;
    d.taitB5 = t.b5; d.taitB6 = t.b6;
    return d;
}

static MouldMaterialData BuiltInMould(const TestMaterial::MouldMaterial& m,
                                      const char* grade, const char* family)
{
    MouldMaterialData d;
    d.name = m.name;
    d.grade = grade;
    d.family = family;
    d.notes = "Mould3r built-in: representative generic values, not certified datasheet data";
    d.density = m.density / 1000.0;
    d.specificHeat = m.specificHeat;
    d.thermalConductivity = m.thermalConductivity;
    return d;
}

MouldMaterialData BuiltInSteel()
{ return BuiltInMould(TestMaterial::kMouldSteel, "P20-class", "tool steel"); }

MouldMaterialData BuiltInAluminum()
{ return BuiltInMould(TestMaterial::kMouldAluminum, "7075-class", "aluminium alloy"); }

// ---------------------------------------------------------------------------
void MaterialLibrary::Reload()
{
    m_injection.clear();
    m_mould.clear();
    m_problems.clear();

    {
        InjectionLibraryEntry e;
        e.builtIn = true;
        e.data = BuiltInPolypropylene();
        e.displayName = e.data.name;
        m_injection.push_back(std::move(e));
    }
    for (MouldMaterialData d : { BuiltInSteel(), BuiltInAluminum() })
    {
        MouldLibraryEntry e;
        e.builtIn = true;
        e.data = std::move(d);
        e.displayName = e.data.name;
        m_mould.push_back(std::move(e));
    }

    std::string err;
    if (!MaterialFile::EnsureLibraryFolders(err))
        m_problems.push_back(err);
    ScanFolder(MaterialKind::Injection, m_injection, m_problems);
    ScanFolder(MaterialKind::Mould, m_mould, m_problems);
}

int MaterialLibrary::FindInjection(const std::string& key) const { return FindKey(m_injection, key); }
int MaterialLibrary::FindMould(const std::string& key) const { return FindKey(m_mould, key); }

bool MaterialLibrary::Fetch(const InjectionLibraryEntry& e, InjectionMaterialData& out,
                            std::string& error, std::vector<std::string>* warnings)
{ return FetchImpl(e, out, error, warnings); }

bool MaterialLibrary::Fetch(const MouldLibraryEntry& e, MouldMaterialData& out,
                            std::string& error, std::vector<std::string>* warnings)
{ return FetchImpl(e, out, error, warnings); }
