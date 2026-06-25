#pragma once

#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// RAII wrapper around a read-only memory-mapped file.
// Calls madvise(MADV_SEQUENTIAL) on open; safe for zero-byte files.
struct MmapFile
{
    const char *data = nullptr;
    size_t size = 0;
    int fd = -1;

    explicit MmapFile(const std::string &path)
    {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("Cannot open: " + path);
        struct stat sb;
        if (fstat(fd, &sb) < 0)
            throw std::runtime_error("Cannot stat: " + path);
        size = (size_t)sb.st_size;
        if (size > 0)
        {
            data = (const char *)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (data == MAP_FAILED)
            {
                close(fd);
                throw std::runtime_error("mmap failed: " + path);
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
        o.fd   = -1;
    }

    std::string_view view() const
    {
        return {data, size};
    }
};
