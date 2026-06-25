#pragma once

#include "graph_read.h"

#include <bit>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

// ─── numDigits ───────────────────────────────────────────────────────────────
//
// Branchless decimal digit count for n >= 1.
// 1233/4096 ≈ log10(2); correction term handles all boundary values exactly.
// Compiles to: lzcnt, imul, shr, cmp, add.

inline uint32_t numDigits(uint32_t n)
{
    static constexpr uint32_t pow10[] = {
        1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u, 1000000000u
    };
    uint32_t t = (std::bit_width(n) * 1233u) >> 12;
    return t + (n >= pow10[t]);
}

// ─── writeGraphToMetis ───────────────────────────────────────────────────────
//
// Two-pass write: first compute per-vertex line byte lengths (to know the
// total file size for pre-allocation), then fill the mmap'd output in a
// second pass. Single-threaded when opts.num_threads == 1 (the default);
// parallel when num_threads > 1 (both passes use the same thread count).

template <class K, class O>
void writeGraphToMetis(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    size_t n = g.span(), m = g.size() / 2;
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    // ── Pass 1: compute per-vertex line byte lengths ──────────────────────────
    std::vector<size_t> line_bytes(n);

    auto computeLineSizes = [&](size_t u_begin, size_t u_end)
    {
        for (size_t u = u_begin; u < u_end; ++u)
        {
            size_t bytes = 1; // newline
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v)
            {
                if (!first)
                    ++bytes; // space separator
                bytes += numDigits((uint32_t)(v + 1));
                first = false;
            });
            line_bytes[u] = bytes;
        }
    };

    if (opts.num_threads <= 1)
    {
        computeLineSizes(0, n);
    }
    else
    {
#pragma omp parallel for num_threads(opts.num_threads) schedule(dynamic, 2048)
        for (size_t u = 0; u < n; ++u)
        {
            size_t bytes = 1;
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v)
            {
                if (!first)
                    ++bytes;
                bytes += numDigits((uint32_t)(v + 1));
                first = false;
            });
            line_bytes[u] = bytes;
        }
    }

    // ── Prefix sum → per-vertex byte offsets ─────────────────────────────────
    std::vector<size_t> line_off(n + 1);
    line_off[0] = (size_t)hlen;
    for (size_t i = 0; i < n; ++i)
        line_off[i + 1] = line_off[i] + line_bytes[i];
    std::vector<size_t>().swap(line_bytes);

    size_t total = line_off[n];
    std::string out_path = output_path + ".metis";
    int fd = open(out_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        throw std::runtime_error("Cannot create: " + out_path);
    if (posix_fallocate(fd, 0, (off_t)total) != 0)
    {
        close(fd);
        throw std::runtime_error("posix_fallocate failed: " + out_path);
    }
    char *buf = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED)
    {
        close(fd);
        throw std::runtime_error("mmap failed: " + out_path);
    }

    memcpy(buf, header, hlen);

    // ── Pass 2: write adjacency lines ─────────────────────────────────────────
    auto writeLines = [&](size_t u_begin, size_t u_end)
    {
        for (size_t u = u_begin; u < u_end; ++u)
        {
            char *p = buf + line_off[u];
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v)
            {
                if (!first)
                    *p++ = ' ';
                auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)(v + 1));
                p = ptr;
                first = false;
            });
            *p = '\n';
        }
    };

    if (opts.num_threads <= 1)
    {
        writeLines(0, n);
    }
    else
    {
#pragma omp parallel for num_threads(opts.num_threads) schedule(dynamic, 2048)
        for (size_t u = 0; u < n; ++u)
        {
            char *p = buf + line_off[u];
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v)
            {
                if (!first)
                    *p++ = ' ';
                auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)(v + 1));
                p = ptr;
                first = false;
            });
            *p = '\n';
        }
    }

    msync(buf, total, MS_SYNC);
    munmap(buf, total);
    close(fd);
}

// ─── writeGraphToParquet ─────────────────────────────────────────────────────
// Milestone 5.

template <class K, class O> void writeGraphToParquet(const DiGraphCsr<K, O> &, const std::string &, const ParseOptions & = {})
{
    throw std::runtime_error("writeGraphToParquet: not yet implemented");
}

// ─── writeGraphToCSV ─────────────────────────────────────────────────────────
// Milestone 5.

template <class K, class O> void writeGraphToCSV(const DiGraphCsr<K, O> &, const std::string &, const ParseOptions & = {})
{
    throw std::runtime_error("writeGraphToCSV: not yet implemented");
}

// ─── writeGraph ──────────────────────────────────────────────────────────────

template <class K, class O>
void writeGraph(const DiGraphCsr<K, O> &g, const std::string &output_path, EdgesFormat fmt,
                const ParseOptions &opts = {})
{
    switch (fmt)
    {
        case METIS:
            writeGraphToMetis(g, output_path, opts);
            return;
        case CSR_PARQUET:
            writeGraphToParquet(g, output_path, opts);
            return;
        case CSV_EDGELIST:
            writeGraphToCSV(g, output_path, opts);
            return;
        default:
            throw std::runtime_error("writeGraph: unknown format");
    }
}
