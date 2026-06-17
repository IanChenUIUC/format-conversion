#pragma once

#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "Graph.hxx"
#include "_main.hxx"
#include "io.hxx"

using namespace std;

struct ConvertOptions
{
    char sep = ',';
    bool node_header = true;
    bool edge_header = true;
    bool weighted = false;
    bool skip_loops = true;
};

// Declared here, defined below. This is the one symbol pybind11 and main() call.
void convert_graph(const string &nodes_file, const string &edges_file, const string &metis_file,
                   const ConvertOptions &opts = {});

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

template <class K>
static void readEdgesParallel(const string &path, const ConvertOptions &opts, vector<vector<K>> &t_src,
                              vector<vector<K>> &t_dst)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1)
        throw runtime_error("Cannot open edges file: " + path);
    struct stat sb;
    fstat(fd, &sb);
    size_t fsz = sb.st_size;
    const char *data = (const char *)mmap(nullptr, fsz, PROT_READ, MAP_PRIVATE, fd, 0);
    madvise((void *)data, fsz, MADV_SEQUENTIAL);
    const char *file_end = data + fsz;
    const char *body = opts.edge_header ? skipLine(data, file_end) : data;
    const int T = omp_get_max_threads();
    t_src.assign(T, {});
    t_dst.assign(T, {});
    size_t body_size = (size_t)(file_end - body);
    size_t chunk = (body_size + T - 1) / T;
#pragma omp parallel num_threads(T)
    {
        int tid = omp_get_thread_num();

        const char *start_ptr = body + (size_t)tid * chunk;
        const char *end_ptr = body + (size_t)(tid + 1) * chunk;
        if (end_ptr > file_end)
            end_ptr = file_end;

        const char *lo = (tid == 0) ? body : skipLine(start_ptr, file_end);
        const char *hi = (tid == T - 1) ? file_end : skipLine(end_ptr, file_end);
        auto &src = t_src[tid];
        auto &dst = t_dst[tid];

        const char *p = lo;
        while (p < hi)
        {
            K u = 0, v = 0;
            if (parseUint(p, file_end, u))
            {
                while (p < hi && *p == opts.sep)
                    ++p;
                if (parseUint(p, hi, v))
                {
                    if (!opts.skip_loops || u != v)
                    {
                        src.push_back(u);
                        src.push_back(v);
                        dst.push_back(v);
                        dst.push_back(u);
                    }
                }
            }
            p = skipLine(p, file_end);
        }
    }

    munmap((void *)data, fsz);
    close(fd);
}

template <class K>
static void translateAndCountDegrees(vector<vector<K>> &t_src, vector<vector<K>> &t_dst,
                                     const unordered_map<K, K> &node_map, size_t N, vector<vector<K>> &t_deg)
{
    const int T = (int)t_src.size();
    t_deg.assign(T, vector<K>(N, 0));
#pragma omp parallel for schedule(dynamic)
    for (int t = 0; t < T; ++t)
    {
        auto &src = t_src[t];
        auto &dst = t_dst[t];
        auto &deg = t_deg[t];
        for (size_t i = 0, n = src.size(); i < n; ++i)
        {
            src[i] = node_map.at(src[i]);
            dst[i] = node_map.at(dst[i]);
            ++deg[src[i]];
        }
    }
}

template <class K, int PARTITIONS>
static void reduceDegrees(const vector<vector<K>> &t_deg, size_t N, vector<K *> &degrees)
{
    for (int t = 0; t < (int)t_deg.size(); ++t)
    {
        const auto &deg = t_deg[t];
        const int p = t % PARTITIONS;

        for (size_t u = 0; u < N; ++u)
        {
            if (deg[u])
                degrees[p][u] += deg[u];
        }
    }
}

template <bool WEIGHTED = false, int PARTITIONS = 1, class G>
static void readCsvToGraphOmpW(G &a, const string &nodes_file, const string &edges_file, const ConvertOptions &opts)
{
    using O = typename G::offset_type;
    using K = typename G::key_type;
    using E = typename G::edge_value_type;
    unordered_map<K, K> node_map;
    const size_t N = buildNodeMap<K>(nodes_file, opts, node_map);
    const int T = omp_get_max_threads();
    vector<vector<K>> t_src, t_dst;
    readEdgesParallel<K>(edges_file, opts, t_src, t_dst);
    size_t M = 0;
    for (int t = 0; t < T; ++t)
        M += t_src[t].size() / 2;

    vector<vector<K>> t_deg;
    translateAndCountDegrees<K>(t_src, t_dst, node_map, N, t_deg);

    vector<size_t> partition_counts(PARTITIONS, 0);
    for (int t = 0; t < T; ++t)
        partition_counts[t % PARTITIONS] += t_src[t].size();

    vector<K *> degrees(PARTITIONS);
    vector<O *> offsets(PARTITIONS);
    vector<K *> edgeKeys(PARTITIONS);
    vector<E *> edgeValues(PARTITIONS);
    for (int p = 0; p < PARTITIONS; ++p)
    {
        degrees[p] = new K[N + 1]();
        offsets[p] = new O[N + 1]();
        edgeKeys[p] = new K[partition_counts[p]];
        edgeValues[p] = WEIGHTED ? new E[partition_counts[p]] : nullptr;
    }
    reduceDegrees<K, PARTITIONS>(t_deg, N, degrees);
    vector<K *> sources(T), targets(T);
    vector<E *> weights(T, nullptr);
    vector<size_t> counts(T);
    for (int t = 0; t < T; ++t)
    {
        sources[t] = t_src[t].data();
        targets[t] = t_dst[t].data();
        counts[t] = t_src[t].size();
    }

    convertEdgelistToGraphOmpW<WEIGHTED, PARTITIONS>(a, offsets, edgeKeys, edgeValues, degrees, sources, targets,
                                                     weights, counts, N);
    for (int p = 0; p < PARTITIONS; ++p)
    {
        delete[] degrees[p];
        delete[] offsets[p];
        delete[] edgeKeys[p];
        if (WEIGHTED)
            delete[] edgeValues[p];
    }
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

template <class G> static void writeGraphToMetis(const G &a, const string &output_file)
{
    using K = typename G::key_type;
    size_t n = a.span(), m = a.size() / 2;
    char header[128];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);
    vector<size_t> line_bytes(n);
#pragma omp parallel for schedule(dynamic, 2048)
    for (K u = 0; u < (K)n; ++u)
    {
        size_t bytes = 1;
        bool first = true;
        a.forEachEdgeKey(u, [&](auto v, auto...) {
            if (!first)
                ++bytes;
            bytes += numDigits((uint32_t)(v + 1));
            first = false;
        });
        line_bytes[u] = bytes;
    }
    vector<size_t> line_off(n + 1);
    line_off[0] = hlen;
    for (size_t i = 0; i < n; ++i)
        line_off[i + 1] = line_off[i] + line_bytes[i];
    size_t total = line_off[n];
    int fd = open(output_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd == -1)
        throw runtime_error("Cannot open METIS output: " + output_file);
    posix_fallocate(fd, 0, (off_t)total);
    char *map_ptr = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_ptr == MAP_FAILED)
    {
        close(fd);
        throw runtime_error("mmap failed");
    }
    memcpy(map_ptr, header, hlen);
#pragma omp parallel for schedule(dynamic, 2048)
    for (K u = 0; u < (K)n; ++u)
    {
        char *p = map_ptr + line_off[u];
        bool first = true;
        a.forEachEdgeKey(u, [&](auto v, auto...) {
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

inline void convert_graph(const string &nodes_file, const string &edges_file, const string &metis_file,
                          const ConvertOptions &opts)
{
    using K = uint32_t;
    using V = None;
    using E = None;
    omp_set_num_threads(omp_get_max_threads());
    ArenaDiGraph<K, V, E> a;
    std::cout << "Reading edgelist..." << std::endl;
    readCsvToGraphOmpW(a, nodes_file, edges_file, opts);
    std::cout << "Writing metis..." << std::endl;
    writeGraphToMetis(a, metis_file);
    std::cout << "Done writing metis..." << std::endl;
}
