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
#include <arrow/ipc/api.h>
#include <parquet/arrow/writer.h>

// ─── numDigits ───────────────────────────────────────────────────────────────
// Branchless decimal digit count for n >= 1.
// 1233/4096 ≈ log10(2); correction term handles boundary values exactly.
// Compiles to: lzcnt, imul, shr, cmp, add.

inline uint32_t numDigits(uint32_t n)
{
    static constexpr uint32_t pow10[] = {1u,      10u,      100u,      1000u,      10000u,
                                         100000u, 1000000u, 10000000u, 100000000u, 1000000000u};
    uint32_t t = (std::bit_width(n) * 1233u) >> 12;
    return t + (n >= pow10[t]);
}

// ─── writeGraphToMetis ───────────────────────────────────────────────────────

template <class K, class O> void writeGraphToMetis(const DiGraphCsr<K, O> &g, const std::string &output_path)
{
    size_t n = g.span(), m = g.size() / 2;
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    std::vector<size_t> line_bytes(n);
    for (size_t u = 0; u < n; ++u)
    {
        size_t bytes = 1; // newline
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                ++bytes;
            bytes += numDigits((uint32_t)(v + 1));
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
    std::string out = output_path + ".metis";
    int fd = open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        throw std::runtime_error("Cannot create: " + out);
    posix_fallocate(fd, 0, (off_t)total);
    char *buf = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED)
    {
        close(fd);
        throw std::runtime_error("mmap failed: " + out);
    }

    memcpy(buf, header, hlen);
    for (size_t u = 0; u < n; ++u)
    {
        char *p = buf + line_off[u];
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                *p++ = ' ';
            auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)(v + 1));
            p = ptr;
            first = false;
        });
        *p = '\n';
    }

    msync(buf, total, MS_SYNC);
    munmap(buf, total);
    close(fd);
}

// ─── writeGraphToParquet ─────────────────────────────────────────────────────

template <class K, class O> void writeGraphToParquet(const DiGraphCsr<K, O> &g, const std::string &output_path)
{
    auto wrapU64 = [](const auto &vec) {
        using T = typename std::remove_reference_t<decltype(vec)>::value_type;
        auto buf = arrow::Buffer::Wrap(vec.data(), vec.size() * sizeof(T));
        auto dat = arrow::ArrayData::Make(arrow::uint64(), (int64_t)vec.size(), {nullptr, buf});
        return arrow::MakeArray(dat);
    };
    auto writeTable = [](std::shared_ptr<arrow::Table> table, const std::string &path) {
        auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
        ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::FileOutputStream::Open(path));
        ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeFileWriter(sink, table->schema()));
        ARROW_RETURN_NOT_OK(writer->WriteTable(*table));
        ARROW_RETURN_NOT_OK(writer->Close());
        return sink->Close();
    };
    writeTable(arrow::Table::Make(arrow::schema({arrow::field("indices", arrow::uint64())}), {wrapU64(g.edgeKeys)}),
               output_path + ".indices.parquet");
    writeTable(arrow::Table::Make(arrow::schema({arrow::field("indptr", arrow::uint64())}), {wrapU64(g.offsets)}),
               output_path + ".indptr.parquet");
}

// ─── writeGraph ──────────────────────────────────────────────────────────────
// Milestone 2 (METIS), Milestone 5 (Parquet, CSV).

template <class K, class O> void writeGraph(const DiGraphCsr<K, O> &g, const std::string &output_path, EdgesFormat fmt)
{
    throw std::runtime_error("writeGraph: not implemented");
}
