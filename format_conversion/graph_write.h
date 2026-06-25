#pragma once

#include "Graph.hxx"
#include "graph_read.h"

#include <bit>
#include <charconv>
#include <fcntl.h>
#include <sys/mman.h>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/ipc/api.h>
#include <parquet/arrow/writer.h>

inline uint32_t numDigits(uint32_t n)
{
    static constexpr uint32_t pow10[] = {1u,      10u,      100u,      1000u,      10000u,
                                         100000u, 1000000u, 10000000u, 100000000u, 1000000000u};
    uint32_t t = (std::bit_width(n) * 1233u) >> 12;
    return t + (n >= pow10[t]);
}

template <class K, class O>
void writeGraph(const DiGraphCsr<K, O> &g, const std::string &output_file, EdgesFormat output_fmt)
{
    // TODO
}

// ─── writeGraphToMetis ───────────────────────────────────────────────────────
//
// Writes the graph in METIS adjacency-list format to output_file + ".metis".
// Uses a parallel pre-pass to compute per-vertex line byte lengths, then
// a single mmap'd output write (no reallocation, no buffering).

template <class K, class O> void writeGraphToMetis(const DiGraphCsr<K, O> &g, const std::string &output_file)
{
    size_t n = g.span(), m = g.size() / 2;
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    std::vector<size_t> line_bytes(n);
#pragma omp parallel for schedule(dynamic, 2048)
    for (size_t u = 0; u < n; ++u)
    {
        size_t bytes = 1; // newline
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                ++bytes; // space separator
            bytes += numDigits(v + 1);
            first = false;
        });
        line_bytes[u] = bytes;
    }

    std::vector<size_t> line_off(n + 1);
    line_off[0] = (size_t)hlen;
    for (size_t i = 0; i < n; ++i)
        line_off[i + 1] = line_off[i] + line_bytes[i];
    std::vector<size_t>().swap(line_bytes);

    size_t total = line_off[n];
    std::string out_path = output_file + ".metis";
    int fd = open(out_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        throw std::runtime_error("Cannot create: " + out_path);
    posix_fallocate(fd, 0, (off_t)total);
    char *out = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out == MAP_FAILED)
    {
        close(fd);
        throw std::runtime_error("mmap output failed");
    }

    memcpy(out, header, hlen);

#pragma omp parallel for schedule(dynamic, 2048)
    for (size_t u = 0; u < n; ++u)
    {
        char *p = out + line_off[u];
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                *p++ = ' ';
            auto [ptr, _] = std::to_chars(p, p + 11, v + 1);
            p = ptr;
            first = false;
        });
        *p = '\n';
    }

    msync(out, total, MS_SYNC);
    munmap(out, total);
    close(fd);
}

// ─── writeGraphToParquet ─────────────────────────────────────────────────────
//
// Writes edgeKeys and offsets as two Parquet files (zero-copy via Buffer::Wrap).

template <class K, class O> void writeGraphToParquet(const DiGraphCsr<K, O> &g, const std::string &output_file)
{
    auto wrapTyped = [](auto type, const auto &vec) {
        using T = typename std::remove_reference_t<decltype(vec)>::value_type;
        auto buf = arrow::Buffer::Wrap(vec.data(), vec.size() * sizeof(T));
        auto data = arrow::ArrayData::Make(type, (int64_t)vec.size(), {nullptr, buf});
        return arrow::MakeArray(data);
    };

    auto writeTable = [](std::shared_ptr<arrow::Table> table, const std::string &path) {
        ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::FileOutputStream::Open(path));
        ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeFileWriter(sink, table->schema()));
        ARROW_RETURN_NOT_OK(writer->WriteTable(*table));
        ARROW_RETURN_NOT_OK(writer->Close());
        return sink->Close();
    };

    // TODO: figure out a nice way to set the type (int32 or int64)
    writeTable(arrow::Table::Make(arrow::schema({arrow::field("indices", arrow::uint64())}),
                                  {wrapTyped(arrow::uint64(), g.edgeKeys)}),
               output_file + ".indices.parquet");

    writeTable(arrow::Table::Make(arrow::schema({arrow::field("indptr", arrow::uint64())}),
                                  {wrapTyped(arrow::uint64(), g.offsets)}),
               output_file + ".indptr.parquet");
}
