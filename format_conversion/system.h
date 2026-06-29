#pragma once

// Low-level system primitives: memory-mapped files, huge-page-backed scratch
// buffers, and a parallel-for that surfaces worker exceptions to the caller.
// Nothing here is graph-specific. See DESIGN.md for the rationale behind the
// memory and threading choices (and the note on NUMA / numactl).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

// Read-only memory-mapped file (RAII). Advises sequential access on open and is
// safe for zero-byte files. `path` is retained for readers that need it post-open.
struct MmapFile
{
    const char *data = nullptr;
    size_t size = 0;
    int fd = -1;
    std::string path;

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

// Uninitialised, 2 MB-aligned buffer advised for transparent huge pages. Used for
// the large random-access scratch in graph building, where huge pages cut TLB
// misses. The caller is responsible for initialising the contents.
template <class T> struct HugeArray
{
    T *p = nullptr;

    explicit HugeArray(size_t count)
    {
        if (posix_memalign(reinterpret_cast<void **>(&p), size_t{1} << 21, count * sizeof(T)) != 0)
            throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
        madvise(p, count * sizeof(T), MADV_HUGEPAGE);
#endif
    }
    ~HugeArray()
    {
        std::free(p);
    }
    HugeArray(const HugeArray &) = delete;
    HugeArray &operator=(const HugeArray &) = delete;

    T *data() const
    {
        return p;
    }
};

// Advise an existing region for transparent huge pages (no-op where unsupported).
inline void adviseHugePages(void *p, size_t bytes)
{
#ifdef MADV_HUGEPAGE
    if (p && bytes)
        madvise(p, bytes, MADV_HUGEPAGE);
#else
    (void)p;
    (void)bytes;
#endif
}

// Run body(t) for t in [0, T) across T threads. The first exception thrown by any
// worker is captured and rethrown after the region, so worker errors surface as
// normal catchable exceptions instead of terminating the process. Static
// scheduling gives iteration t a stable thread, which callers may rely on.
template <class Body> inline void parallelStripes(int T, Body &&body)
{
    std::exception_ptr eptr;
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; ++t)
    {
        try
        {
            body(t);
        }
        catch (...)
        {
#pragma omp critical
            {
                if (!eptr)
                    eptr = std::current_exception();
            }
        }
    }
    if (eptr)
        std::rethrow_exception(eptr);
}
