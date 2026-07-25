#pragma once

// Core types shared across the API: one options struct per on-disk format per
// direction, the variant that carries them, and the descriptors passed into
// convert / partition.
//
// A spec is both the format tag and the options for it: `CsvEdgelistRead` says
// "read a CSV edge list" and carries exactly the settings that path consults.
// Fields a format does not use are absent rather than ignored. See DESIGN.md.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <Graph.hxx>

#include "system.h"

// ── Read specs ──────────────────────────────────────────────────────────────
//
//   base_index      : subtracted from every raw id (e.g. 1 for 1-indexed input).
//   keep_self_loops : retain u==u edges instead of dropping them.
//   directed        : false stores both directions of each edge; true stores
//                     only the arc u->v.

struct CsvEdgelistRead
{
    char sep = ',';           // ',' | '\t' | ' '
    char comment_char = '#';  // '#' | '%'
    size_t skip_rows = 0;
    uint64_t base_index = 0;
    bool keep_self_loops = false;
    bool directed = false;
};

struct MetisRead
{
    char comment_char = '#';
};

// symmetric: whether the file holds both directions of each edge. No CSR file
// records this, and writers that must reject a half-stored graph (METIS) depend
// on it, so the caller declares it.
struct CsrParquetRead
{
    std::string indices_col = "indices";
    std::string indptr_col = "indptr";
    bool symmetric = true;
};

struct EdgelistParquetRead
{
    std::string source_col = "source";
    std::string target_col = "target";
    uint64_t base_index = 0;
    bool keep_self_loops = false;
    bool directed = false;
};

// ── Write specs ─────────────────────────────────────────────────────────────
//
// Whether a row is emitted once per edge or once per arc follows the graph, not
// the spec: a symmetric CSR emits each edge once as u,v with u<v, and a graph
// built with directed=true emits every stored arc.
//
//   expand_symmetric : emit both u,v and v,u for each edge of a symmetric graph.
//   u64_indices /
//   u64_ids          : widen the emitted id column(s) from uint32 to uint64, to
//                      match tools that store 64-bit node ids (e.g. icebug).

struct CsvEdgelistWrite
{
    char sep = ',';
    bool expand_symmetric = false;
};

struct MetisWrite
{
};

struct CsrParquetWrite
{
    std::string indices_col = "indices";
    std::string indptr_col = "indptr";
    bool u64_indices = false;
};

struct EdgelistParquetWrite
{
    std::string source_col = "source";
    std::string target_col = "target";
    bool u64_ids = false;
    bool expand_symmetric = false;
};

// ── Auxiliary text inputs ───────────────────────────────────────────────────

struct NodelistCsv
{
    char comment_char = '#';
    size_t skip_rows = 0;
    uint64_t base_index = 0;
};

struct LabelsCsv
{
    char comment_char = '#';
    size_t skip_rows = 0;
};

// ── Spec variant ────────────────────────────────────────────────────────────

// Read alternatives come first; isReadSpec depends on that ordering.
using GraphSpec = std::variant<CsvEdgelistRead, MetisRead, CsrParquetRead, EdgelistParquetRead,
                               CsvEdgelistWrite, MetisWrite, CsrParquetWrite, EdgelistParquetWrite>;

inline constexpr size_t NUM_READ_SPECS = 4;

inline bool isReadSpec(const GraphSpec &s)
{
    return s.index() < NUM_READ_SPECS;
}

inline bool isWriteSpec(const GraphSpec &s)
{
    return !isReadSpec(s);
}

// The Python-side name of a spec, for error messages.
inline std::string specName(const GraphSpec &s)
{
    return std::visit(
        [](auto &&v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, CsvEdgelistRead>)
                return "CsvEdgelist.Read";
            else if constexpr (std::is_same_v<T, MetisRead>)
                return "Metis.Read";
            else if constexpr (std::is_same_v<T, CsrParquetRead>)
                return "CsrParquet.Read";
            else if constexpr (std::is_same_v<T, EdgelistParquetRead>)
                return "EdgelistParquet.Read";
            else if constexpr (std::is_same_v<T, CsvEdgelistWrite>)
                return "CsvEdgelist.Write";
            else if constexpr (std::is_same_v<T, MetisWrite>)
                return "Metis.Write";
            else if constexpr (std::is_same_v<T, CsrParquetWrite>)
                return "CsrParquet.Write";
            else
                return "EdgelistParquet.Write";
        },
        s);
}

// Whether reading through this spec yields a CSR holding both directions of each
// edge. A pure function of the spec, so it is available before the build.
inline bool readSymmetric(const GraphSpec &s)
{
    return std::visit(
        [](auto &&v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, CsvEdgelistRead> || std::is_same_v<T, EdgelistParquetRead>)
                return !v.directed;
            else if constexpr (std::is_same_v<T, MetisRead>)
                return true;
            else if constexpr (std::is_same_v<T, CsrParquetRead>)
                return v.symmetric;
            else
                throw std::logic_error("readSymmetric: not a read spec");
        },
        s);
}

// ── Descriptors ─────────────────────────────────────────────────────────────

// An input node list (provides id remapping and any extra node columns). The
// mapping backs NodeMap::getRow, so it must outlive every graph built from it.
struct NodeDescriptor
{
    MmapFile mmap;
    NodelistCsv spec;

    NodeDescriptor(const std::string &path, NodelistCsv spec = {}) : mmap(path), spec(std::move(spec))
    {
    }
};

// A graph file and the spec naming its format. Serves both sides: with a read
// spec the path is an existing file, with a write spec it is the prefix the
// writer appends its extension to.
struct GraphDescriptor
{
    std::string path;
    GraphSpec spec;

    GraphDescriptor(std::string path, GraphSpec spec) : path(std::move(path)), spec(std::move(spec))
    {
        if (isReadSpec(this->spec) && !fileReadable(this->path))
            throw std::runtime_error("Cannot open: " + this->path);
    }
};

// Surfaced to Python as NotImplementedError. For options that are accepted but
// not yet honoured, so the gap is named rather than looking like a typo.
struct NotImplemented : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Reject a descriptor whose spec belongs to the other side, naming it.
inline void requireSide(const GraphDescriptor &gd, bool want_read, const char *fn)
{
    if (isReadSpec(gd.spec) == want_read)
        return;
    throw std::runtime_error(std::string(fn) + ": " + (want_read ? "input" : "output") + " descriptor carries a " +
                             (want_read ? "write" : "read") + " spec (" + specName(gd.spec) + "); expected a " +
                             (want_read ? "read" : "write") + " spec");
}

// ── Built graph ─────────────────────────────────────────────────────────────

// The CSR plus the one property no on-disk format records.
template <class K, class O>
struct BuiltGraph
{
    DiGraphCsr<K, O> g;
    bool symmetric = true;
};
