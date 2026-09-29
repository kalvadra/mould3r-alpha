#pragma once
// ===========================================================================
// MaterialLibrary.h — the materials offered in the Physical Setup dropdowns.
//
// Built-in materials first (the representative TestMaterial figures, held in
// code so a missing or emptied library folder never leaves the simulations
// without a material), then every *.material file in
//   Materials/Injection Materials   and   Materials/Mould Materials
// (see MaterialFile.h), sorted by name.
//
// The library keeps the data read at the last scan for listing; Fetch re-reads
// a file entry from disk, so edits made in a text editor since the scan are
// picked up when a simulation runs.
// ===========================================================================

#include <string>
#include <vector>

#include "MaterialFile.h"

template <class Data>
struct MaterialLibraryEntry
{
    std::string displayName;   // unique within its list (UTF-8)
    std::string path;          // file path (native narrow encoding, as MaterialFile takes); empty for a built-in
    bool        builtIn = false;
    Data        data;          // as read at the last scan

    // Stable identity for keeping a selection across rescans.
    std::string Key() const { return builtIn ? "builtin:" + displayName : "file:" + path; }
};

using InjectionLibraryEntry = MaterialLibraryEntry<InjectionMaterialData>;
using MouldLibraryEntry     = MaterialLibraryEntry<MouldMaterialData>;

// The built-in materials, as library data (every value filled, so they resolve
// with no derivations or fallbacks).
InjectionMaterialData BuiltInPolypropylene();
MouldMaterialData     BuiltInSteel();
MouldMaterialData     BuiltInAluminum();

class MaterialLibrary
{
public:
    // Rebuild both lists: built-ins + a scan of the library folders (created
    // if missing). Unreadable / wrong-kind files are skipped and reported in
    // ScanProblems().
    void Reload();

    const std::vector<InjectionLibraryEntry>& Injection() const { return m_injection; }
    const std::vector<MouldLibraryEntry>&     Mould() const { return m_mould; }
    const std::vector<std::string>&           ScanProblems() const { return m_problems; }

    // Index of the entry with this Key(), or -1.
    int FindInjection(const std::string& key) const;
    int FindMould(const std::string& key) const;

    // Current data for an entry (built-in: from memory; file: re-read now).
    // Load warnings (malformed values, ...) go to `warnings` when given.
    static bool Fetch(const InjectionLibraryEntry& e, InjectionMaterialData& out,
                      std::string& error, std::vector<std::string>* warnings = nullptr);
    static bool Fetch(const MouldLibraryEntry& e, MouldMaterialData& out,
                      std::string& error, std::vector<std::string>* warnings = nullptr);

private:
    std::vector<InjectionLibraryEntry> m_injection;
    std::vector<MouldLibraryEntry>     m_mould;
    std::vector<std::string>           m_problems;
};
