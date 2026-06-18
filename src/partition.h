#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <omp.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tlx/cmdline_parser.hpp>
#include <tlx/string.hpp>

namespace fs = std::filesystem;

void partition(const std::string &nodelist_path, const std::string &labels_path, const std::string &metis_path,
               const std::string &out_dir);

class MemoryMappedFile
{
  public:
    int fd = -1;
    size_t size = 0;
    const char *data = nullptr;

    MemoryMappedFile(const std::string &path)
    {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("Failed to open: " + path);

        struct stat st;
        if (fstat(fd, &st) < 0)
            throw std::runtime_error("Failed to stat: " + path);
        size = st.st_size;

        if (size > 0)
        {
            data = (const char *)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (data == MAP_FAILED)
                throw std::runtime_error("Failed to mmap: " + path);
            madvise((void *)data, size, MADV_SEQUENTIAL);
        }
    }

    ~MemoryMappedFile()
    {
        if (size > 0 && data != MAP_FAILED)
            munmap((void *)data, size);
        if (fd >= 0)
            close(fd);
    }
};

class NodeRegistry
{
  private:
    const MemoryMappedFile &file_;
    std::vector<uint64_t> line_offsets_;
    std::vector<int32_t> labels_;
    std::unordered_set<int32_t> unique_labels_;

  public:
    NodeRegistry(const MemoryMappedFile &file) : file_(file)
    {
    }

    void IndexNodelist()
    {
        line_offsets_.push_back(0);
        const char *p = file_.data;
        const char *end = file_.data + file_.size;
        while (p < end)
        {
            const char *nl = (const char *)memchr(p, '\n', end - p);
            if (!nl)
                break;
            line_offsets_.push_back(nl - file_.data + 1);
            p = nl + 1;
        }
    }

    uint64_t GetNumNodes() const
    {
        return line_offsets_.size() > 1 ? line_offsets_.size() - 2 : 0;
    }

    std::string_view GetHeaderRow() const
    {
        return GetRow(0);
    }

    // Returns entire CSV row given 0-indexed vertex compact ID
    inline std::string_view GetRow(uint64_t compact_id) const
    {
        uint64_t start = line_offsets_[compact_id];
        uint64_t end = line_offsets_[compact_id + 1];
        const char *s_ptr = file_.data + start;
        const char *nl = (const char *)memchr(s_ptr, '\n', end - start);
        return nl ? std::string_view(s_ptr, nl - s_ptr) : std::string_view(s_ptr, end - start);
    }

    // Uses tlx to split the row and grab the first column
    inline std::string_view GetNodeId(uint64_t compact_id) const
    {
        std::string_view row = GetRow(compact_id + 1); // +1 skips header row
        auto columns = tlx::split_view(row, ',');
        return columns.empty() ? row : columns.front();
    }

    void ParseLabels(const MemoryMappedFile &labels_file)
    {
        uint64_t num_nodes = GetNumNodes();
        labels_.assign(num_nodes, -1);

        std::string_view label_data(labels_file.data, labels_file.size);
        auto lines = tlx::split_view(label_data, '\n');

        uint64_t idx = 0;
        for (std::string_view line : lines)
        {
            if (idx >= num_nodes)
                break;

            // Trim whitespace and parse using tlx
            std::string_view trimmed = tlx::trim(line);
            if (trimmed.empty())
                continue;

            int32_t val = 0;
            if (tlx::parse_int32(trimmed, &val))
            {
                labels_[idx++] = val;
                unique_labels_.insert(val);
            }
        }
    }

    inline int32_t GetLabel(uint64_t compact_id) const
    {
        return labels_[compact_id];
    }
    const std::unordered_set<int32_t> &GetUniqueLabels() const
    {
        return unique_labels_;
    }
};

// ============================================================================
// 3. PARALLEL GRAPH STREAMER
// ============================================================================
class GraphStreamer
{
  private:
    const MemoryMappedFile &metis_file_;
    const NodeRegistry &registry_;
    std::string out_dir_;
    std::unordered_map<int32_t, std::unique_ptr<std::mutex>> label_mutexes_;

  public:
    GraphStreamer(const MemoryMappedFile &metis, const NodeRegistry &reg, std::string out)
        : metis_file_(metis), registry_(reg), out_dir_(std::move(out))
    {
        for (int32_t lab : registry_.GetUniqueLabels())
        {
            label_mutexes_[lab] = std::make_unique<std::mutex>();
        }
    }

    void StreamEdges()
    {
        const char *m_data = metis_file_.data;
        size_t m_size = metis_file_.size;

        // Advance past METIS header line
        const char *m_start = (const char *)memchr(m_data, '\n', m_size);
        m_start = m_start ? m_start + 1 : m_data + m_size;
        size_t data_size = (m_data + m_size) - m_start;

        int num_threads = omp_get_max_threads();
        std::vector<const char *> chunk_bounds(num_threads + 1);
        chunk_bounds[0] = m_start;
        chunk_bounds[num_threads] = m_start + data_size;

        // Establish structural chunks bounded safely by newlines
        for (int i = 1; i < num_threads; ++i)
        {
            size_t approx_offset = (data_size / num_threads) * i;
            const char *p = m_start + approx_offset;
            const char *nl = (const char *)memchr(p, '\n', (m_start + data_size) - p);
            chunk_bounds[i] = nl ? nl + 1 : m_start + data_size;
        }

        std::vector<uint64_t> chunk_u_starts(num_threads + 1, 0);

// Pass 1: Compute explicit start-vertex index mapping for each thread
#pragma omp parallel for
        for (int i = 0; i < num_threads; ++i)
        {
            uint64_t lines = 0;
            const char *p = chunk_bounds[i];
            while (p < chunk_bounds[i + 1])
            {
                const char *nl = (const char *)memchr(p, '\n', chunk_bounds[i + 1] - p);
                if (!nl)
                    break;
                lines++;
                p = nl + 1;
            }
            chunk_u_starts[i + 1] = lines;
        }

        for (int i = 1; i <= num_threads; ++i)
        {
            chunk_u_starts[i] += chunk_u_starts[i - 1];
        }

// Pass 2: Stream, extract, and write
#pragma omp parallel
        {
            int t = omp_get_thread_num();
            const char *p = chunk_bounds[t];
            const char *end = chunk_bounds[t + 1];
            uint64_t u = chunk_u_starts[t];
            uint64_t max_nodes = registry_.GetNumNodes();

            std::unordered_map<int32_t, std::string> local_buffers;
            const size_t FLUSH_LIMIT = 8 * 1024 * 1024; // 8MB optimal I/O block size

            auto flush_buffer = [&](int32_t lab, bool force) {
                auto &buf = local_buffers[lab];
                if (buf.empty() || (!force && buf.size() < FLUSH_LIMIT))
                    return;

                std::lock_guard<std::mutex> lock(*label_mutexes_[lab]);
                fs::path edge_file = fs::path(out_dir_) / std::to_string(lab) / "edgelist.csv";
                std::ofstream out(edge_file, std::ios::app | std::ios::binary);
                out.write(buf.data(), buf.size());
                buf.clear();
            };

            while (p < end && u < max_nodes)
            {
                const char *nl = (const char *)memchr(p, '\n', end - p);
                if (!nl)
                    nl = end;

                int32_t L_u = registry_.GetLabel(u);
                std::string_view str_u;
                bool fetched_u = false;

                // Read line and use tlx to process neighbors
                std::string_view line(p, nl - p);
                auto neighbors = tlx::split_view(line, ' ');

                for (std::string_view token : neighbors)
                {
                    if (token.empty())
                        continue; // Handles consecutive spaces safely

                    uint64_t v = 0;
                    if (tlx::parse_uint64(token, &v) && v > 0)
                    {
                        uint64_t v_idx = v - 1; // 1-indexed conversion
                        if (u < v_idx && registry_.GetLabel(v_idx) == L_u)
                        {
                            if (!fetched_u)
                            {
                                str_u = registry_.GetNodeId(u);
                                fetched_u = true;
                            }
                            std::string_view str_v = registry_.GetNodeId(v_idx);

                            auto &buf = local_buffers[L_u];
                            buf.append(str_u);
                            buf.push_back(',');
                            buf.append(str_v);
                            buf.push_back('\n');

                            flush_buffer(L_u, false);
                        }
                    }
                }

                u++;
                p = (nl < end) ? nl + 1 : end;
            }

            for (auto &[lab, buf] : local_buffers)
            {
                flush_buffer(lab, true);
            }
        }
    }
};
void partition(const std::string &nodelist_path, const std::string &labels_path, const std::string &metis_path,
               const std::string &out_dir)
{
    try
    {
        fs::create_directories(out_dir);

        std::cout << "[1/4] Mapping files via virtual memory..." << std::endl;
        MemoryMappedFile nodelist_mmap(nodelist_path);
        MemoryMappedFile labels_mmap(labels_path);
        MemoryMappedFile metis_mmap(metis_path);

        std::cout << "[2/4] Scanning and indexing node boundaries..." << std::endl;
        NodeRegistry registry(nodelist_mmap);
        registry.IndexNodelist();
        registry.ParseLabels(labels_mmap);

        std::cout << "[3/4] Initializing output structure directories..." << std::endl;
        std::string_view header = registry.GetHeaderRow();
        for (int32_t lab : registry.GetUniqueLabels())
        {
            fs::path lab_dir = fs::path(out_dir) / std::to_string(lab);
            fs::create_directories(lab_dir);

            // Clean-truncate edgelist before streaming
            std::ofstream(lab_dir / "edgelist.csv", std::ios::trunc).close();

            // Direct file output stream for sub-nodelists
            std::ofstream nl_out(lab_dir / "nodelist.csv", std::ios::binary);
            nl_out << header << "\n";
            for (uint64_t u = 0; u < registry.GetNumNodes(); ++u)
            {
                if (registry.GetLabel(u) == lab)
                {
                    nl_out << registry.GetRow(u + 1) << "\n";
                }
            }
        }

        std::cout << "[4/4] Executing parallel edge extraction..." << std::endl;
        GraphStreamer streamer(metis_mmap, registry, out_dir);
        streamer.StreamEdges();

        std::cout << "Success! Target files generated cleanly." << std::endl;
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error(std::string("Graph filtering failed: ") + e.what());
    }
}
