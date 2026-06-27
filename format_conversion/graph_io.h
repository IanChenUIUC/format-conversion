#pragma once

#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Graph.hxx>
#include <io.hxx>

#ifdef _OPENMP
#include <omp.h>
#endif

// RAII wrapper around a read-only memory-mapped file.
// Calls madvise(MADV_SEQUENTIAL) on open; safe for zero-byte files.
struct MmapFile
{
    const char *data = nullptr;
    size_t size = 0;
    int fd = -1;
    std::string path; // stored so readers that need the path post-open can access it

    explicit MmapFile(const std::string &p) : path(p)
    {
        fd = open(p.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("Cannot open: " + p);
        struct stat sb;
        if (fstat(fd, &sb) < 0)
            throw std::runtime_error("Cannot stat: " + p);
        size = (size_t)sb.st_size;
        if (size > 0)
        {
            data = (const char *)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (data == MAP_FAILED)
            {
                close(fd);
                throw std::runtime_error("mmap failed: " + p);
            }
            madvise((void *)data, size, MADV_SEQUENTIAL);
        }
    }

    ~MmapFile()
    {
        if (size > 0 && data != MAP_FAILED)
            munmap((void *)data, size);
        if (fd >= 0)
            close(fd);
    }

    MmapFile(const MmapFile &) = delete;
    MmapFile &operator=(const MmapFile &) = delete;

    MmapFile(MmapFile &&o) noexcept : data(o.data), size(o.size), fd(o.fd)
    {
        o.data = nullptr;
        o.size = 0;
        o.fd = -1;
    }

    std::string_view view() const
    {
        return {data, size};
    }
};

// ─── EdgesFormat  ─────────────────────────────────────────────────────────────

enum EdgesFormat
{
    CSV_EDGELIST,
    METIS,
    CSR_PARQUET
};

// ─── ParseOptions ────────────────────────────────────────────────────────────

struct ParseOptions
{
    char sep = ',';
    char comment_char = '#';
    bool keep_self_loops = false;
    size_t skip_rows = 0;
    size_t num_threads = 1;
    uint64_t base_index = 0;
    size_t id_column = 0;
    size_t label_column = 0;
    bool sort_neighbors = false;
};

// ─── Descriptors ─────────────────────────────────────────────────────────────

struct NodeDescriptor
{
    MmapFile mmap;
    ParseOptions opts;

    NodeDescriptor(const std::string &path, ParseOptions opts = {}) : mmap(path), opts(std::move(opts))
    {
    }
};

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
