#pragma once

// Core types shared across the API: the supported edge formats, the options that
// control parsing and output, and the file descriptors passed into convert /
// partition.

#include <cstdint>
#include <string>
#include <utility>

#include "system.h"

// Supported on-disk edge representations.
enum EdgesFormat
{
    CSV_EDGELIST,
    METIS,
    CSR_PARQUET
};

// Parsing and output options.
//   sep, comment_char : field separator and comment marker for text inputs
//                       (sep \in {',', '\t', ' '}, comment_char \in {'#', '%'}).
//   keep_self_loops   : retain u==u edges instead of dropping them.
//   skip_rows         : header lines to skip at the top of an input file.
//   num_threads       : worker threads for the CSV→CSR build (and the sort, when
//                       sort_neighbors is set). 1 = serial.
//   base_index        : value subtracted from every raw id (e.g. 1 for 1-indexed).
//   sort_neighbors    : sort each vertex's adjacency list in the output.
//   use_u64_indices   : emit the CSR_PARQUET indices column as uint64 instead of
//                       uint32 (indptr is already 64-bit). Needed to match tools
//                       that store 64-bit node ids (e.g. the icebug benchmark).
struct ParseOptions
{
    char sep = ',';
    char comment_char = '#';
    bool keep_self_loops = false;
    size_t skip_rows = 0;
    size_t num_threads = 1;
    uint64_t base_index = 0;
    bool sort_neighbors = false;
    bool use_u64_indices = false;
};

// An input node list (provides id remapping and any extra node columns).
struct NodeDescriptor
{
    MmapFile mmap;
    ParseOptions opts;

    NodeDescriptor(const std::string &path, ParseOptions opts = {}) : mmap(path), opts(std::move(opts))
    {
    }
};

// An input edge list / graph file, together with its format and options.
struct GraphDescriptor
{
    MmapFile mmap;
    EdgesFormat fmt;
    ParseOptions opts;

    GraphDescriptor(const std::string &path, EdgesFormat fmt, ParseOptions opts = {})
        : mmap(path), fmt(fmt), opts(std::move(opts))
    {
    }
};
