#pragma once

#include <atomic>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace std;

enum OUTPUT_TYPE
{
    METIS,
    CSR
};

struct ConvertOptions
{
    char sep = ',';
    bool node_header = true;
    bool edge_header = true;
    bool weighted = false;
    bool skip_loops = true;
    OUTPUT_TYPE type;
};

void convert_graph(const string &nodes_file, const string &edges_file, const string &output_file,
                   const ConvertOptions &opts = {});

template <class K = uint32_t, class O = uint64_t> struct CSRGraph
{
    size_t n = 0;
    vector<O> offsets; // size n+1
    vector<K> edges;   // size 2m

    size_t span() const
    {
        return n;
    }
    size_t size() const
    {
        return edges.size();
    }

    template <class F> void forEachEdgeKey(K u, F &&f) const
    {
        for (O i = offsets[u]; i < offsets[u + 1]; ++i)
            f(edges[i]);
    }
};

template <class K> inline bool parseUint(const char *&p, const char *end, K &out)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    if (p >= end || !isdigit((unsigned char)*p))
        return false;
    K v = 0;
    while (p < end && isdigit((unsigned char)*p))
        v = v * 10 + (*p++ - '0');
    out = v;
    return true;
}

inline const char *skipLine(const char *p, const char *end)
{
    while (p < end && *p != '\n')
        ++p;
    return (p < end) ? p + 1 : end;
}

struct MmapFile
{
    const char *data = nullptr;
    size_t size = 0;
    int fd = -1;

    explicit MmapFile(const string &path)
    {
        fd = open(path.c_str(), O_RDONLY);
        if (fd == -1)
            throw runtime_error("Cannot open: " + path);
        struct stat sb;
        fstat(fd, &sb);
        size = (size_t)sb.st_size;
        data = (const char *)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED)
        {
            close(fd);
            throw runtime_error("mmap failed: " + path);
        }
    }
    ~MmapFile()
    {
        if (data && data != MAP_FAILED)
            munmap((void *)data, size);
        if (fd >= 0)
            close(fd);
    }
    MmapFile(const MmapFile &) = delete;
    MmapFile &operator=(const MmapFile &) = delete;
};

template <class K>
static size_t buildNodeMap(const string &path, const ConvertOptions &opts, unordered_map<K, K> &node_map)
{
    ifstream f(path);
    if (!f)
        throw runtime_error("Cannot open nodes file: " + path);
    string line;
    if (opts.node_header)
        getline(f, line);
    size_t N = 0;
    while (getline(f, line))
    {
        if (line.empty())
            continue;
        K id = 0;
        const char *p = line.data(), *end = p + line.size();
        if (!parseUint(p, end, id))
            continue;
        if (node_map.emplace(id, (K)N).second)
            ++N;
    }
    return N;
}

template <class K, class O>
static CSRGraph<K, O> buildCSRFromEdges(const string &edges_file, const ConvertOptions &opts,
                                        const unordered_map<K, K> &node_map, size_t N)
{
    const int T = omp_get_max_threads();
    MmapFile mf(edges_file);
    const char *file_end = mf.data + mf.size;
    const char *body = opts.edge_header ? skipLine(mf.data, file_end) : mf.data;

    size_t body_size = (size_t)(file_end - body);
    size_t chunk = (body_size + T - 1) / T;

    vector<K> degree(N, 0);
    madvise((void *)mf.data, mf.size, MADV_SEQUENTIAL);

#pragma omp parallel num_threads(T)
    {
        int tid = omp_get_thread_num();
        const char *lo = (tid == 0) ? body : skipLine(body + (size_t)tid * chunk, file_end);
        const char *hi = (tid == T - 1) ? file_end : skipLine(body + (size_t)(tid + 1) * chunk, file_end);
        const char sep = opts.sep;
        const char *p = lo;

        while (p < hi)
        {
            K raw_u = 0, raw_v = 0;
            if (!parseUint(p, file_end, raw_u))
            {
                p = skipLine(p, file_end);
                continue;
            }
            while (p < file_end && *p == sep)
                ++p;
            if (!parseUint(p, file_end, raw_v))
            {
                p = skipLine(p, file_end);
                continue;
            }
            p = skipLine(p, file_end);

            auto iu = node_map.find(raw_u), iv = node_map.find(raw_v);
            if (iu == node_map.end() || iv == node_map.end())
                continue;
            K u = iu->second, v = iv->second;
            if (opts.skip_loops && u == v)
                continue;

            __atomic_fetch_add(&degree[u], (K)1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&degree[v], (K)1, __ATOMIC_RELAXED);
        }
    }

    CSRGraph<K, O> g;
    g.n = N;
    g.offsets.resize(N + 1);
    O running = 0;
    for (size_t u = 0; u < N; ++u)
    {
        g.offsets[u] = running;
        running += degree[u];
    }
    g.offsets[N] = running;

    {
        vector<K>().swap(degree);
    } // free

    g.edges.resize((size_t)running);

    vector<O> write_pos(g.offsets.begin(), g.offsets.begin() + N);

    madvise((void *)mf.data, mf.size, MADV_SEQUENTIAL);

#pragma omp parallel num_threads(T)
    {
        int tid = omp_get_thread_num();
        const char *lo = (tid == 0) ? body : skipLine(body + (size_t)tid * chunk, file_end);
        const char *hi = (tid == T - 1) ? file_end : skipLine(body + (size_t)(tid + 1) * chunk, file_end);
        const char sep = opts.sep;
        const char *p = lo;

        while (p < hi)
        {
            K raw_u = 0, raw_v = 0;
            if (!parseUint(p, file_end, raw_u))
            {
                p = skipLine(p, file_end);
                continue;
            }
            while (p < file_end && *p == sep)
                ++p;
            if (!parseUint(p, file_end, raw_v))
            {
                p = skipLine(p, file_end);
                continue;
            }
            p = skipLine(p, file_end);

            auto iu = node_map.find(raw_u), iv = node_map.find(raw_v);
            if (iu == node_map.end() || iv == node_map.end())
                continue;
            K u = iu->second, v = iv->second;
            if (opts.skip_loops && u == v)
                continue;

            O slot_u = __atomic_fetch_add(&write_pos[u], (O)1, __ATOMIC_RELAXED);
            g.edges[slot_u] = v;
            O slot_v = __atomic_fetch_add(&write_pos[v], (O)1, __ATOMIC_RELAXED);
            g.edges[slot_v] = u;
        }
    }
    return g;
}

inline uint32_t numDigits(uint32_t n)
{
    if (n < 10u)
        return 1;
    if (n < 100u)
        return 2;
    if (n < 1000u)
        return 3;
    if (n < 10000u)
        return 4;
    if (n < 100000u)
        return 5;
    if (n < 1000000u)
        return 6;
    if (n < 10000000u)
        return 7;
    if (n < 100000000u)
        return 8;
    if (n < 1000000000u)
        return 9;
    return 10;
}

template <class K, class O> static void writeGraphToMetis(const CSRGraph<K, O> &g, const string &output_file)
{
    size_t n = g.span(), m = g.size() / 2;
    char header[128];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    vector<size_t> line_bytes(n);
#pragma omp parallel for schedule(dynamic, 2048)
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

    vector<size_t> line_off(n + 1);
    line_off[0] = (size_t)hlen;
    for (size_t i = 0; i < n; ++i)
        line_off[i + 1] = line_off[i] + line_bytes[i];
    {
        vector<size_t>().swap(line_bytes);
    } // free

    size_t total = line_off[n];
    string metis_file = output_file + ".metis";
    int fd = open(metis_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd == -1)
        throw runtime_error("Cannot open METIS output: " + metis_file);
    posix_fallocate(fd, 0, (off_t)total);
    char *map_ptr = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_ptr == MAP_FAILED)
    {
        close(fd);
        throw runtime_error("mmap failed for output");
    }

    memcpy(map_ptr, header, hlen);

#pragma omp parallel for schedule(dynamic, 2048)
    for (size_t u = 0; u < n; ++u)
    {
        char *p = map_ptr + line_off[u];
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                *p++ = ' ';
            auto [ptr, _] = to_chars(p, p + 11, (uint32_t)(v + 1));
            p = ptr;
            first = false;
        });
        *p = '\n';
    }

    msync(map_ptr, total, MS_SYNC);
    munmap(map_ptr, total);
    close(fd);
}

template <class T> static std::shared_ptr<arrow::Array> makeUInt64Array(const std::vector<T> &values)
{
    arrow::UInt64Builder builder;
    ARROW_THROW_NOT_OK(builder.Reserve(values.size()));

    for (auto x : values)
        ARROW_THROW_NOT_OK(builder.Append(static_cast<uint64_t>(x)));

    auto result = builder.Finish();
    if (!result.ok())
        throw std::runtime_error(result.status().ToString());

    return *result;
}

static void writeSingleColumnParquet(const std::vector<std::uint64_t> &values, const std::string &column_name,
                                     const std::string &path)
{
    auto array = makeInt64Array(values);
    auto schema = arrow::schema({arrow::field(column_name, arrow::int64())});

    auto table = arrow::Table::Make(schema, {array});

    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    auto st = arrow::io::FileOutputStream::Open(path, &outfile);
    if (!st.ok())
        throw std::runtime_error("Cannot open output file: " + path + " : " + st.ToString());

    st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile);
    if (!st.ok())
        throw std::runtime_error("Failed to write Parquet file: " + path + " : " + st.ToString());
}

template <class K, class O> static void writeGraphToParquet(const CSRGraph<K, O> &g, const std::string &output_file)
{
    const std::string indices_file = output_file + ".indices.parquet";
    const std::string indptr_file = output_file + ".indptr.parquet";

    writeSingleColumnParquet(g.edges, "indices", indices_file);
    writeSingleColumnParquet(g.offsets, "indptr", indptr_file);
}

template <class K>
inline void convert_graph_impl(const string &nodes_file, const string &edges_file, const string &output_file,
                               const ConvertOptions &opts)
{
    using O = uint64_t;

    omp_set_num_threads(omp_get_max_threads());

    unordered_map<K, K> node_map;
    cout << "Building node map..." << endl;
    size_t N = buildNodeMap<K>(nodes_file, opts, node_map);
    cout << "  Nodes: " << N << endl;

    cout << "Building CSR (2-pass streaming)..." << endl;
    CSRGraph<K, O> g = buildCSRFromEdges<K, O>(edges_file, opts, node_map, N);
    cout << "  Undirected edges: " << g.size() / 2 << endl;

    {
        unordered_map<K, K>().swap(node_map);
    } // free

    if (opts.type == METIS)
    {
        cout << "Writing METIS..." << endl;
        writeGraphToMetis(g, output_file);
    }
    else if (opts.type == CSR)
    {
        cout << "Writing CSR..." << endl;
        writeGraphToCSR(g, output_file);
    }
    cout << "Done." << endl;
}

inline void convert_graph(const string &nodes_file, const string &edges_file, const string &output_file,
                          const ConvertOptions &opts)
{
    if (opts.type == CSR)
        convert_graph_impl<uint64_t>(nodes_file, edges_file, output_file, opts);
    else if (opts.type == METIS)
        convert_graph_impl<uint32_t>(nodes_file, edges_file, output_file, opts);
}
