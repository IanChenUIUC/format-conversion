#include "convert.h"
#include <iostream>

[[noreturn]] static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <nodes.csv> <edges.csv> <out.metis>\n"
            "  --sep=<char>        Field separator (default ',')\n"
            "  --sep=tab           Tab separator\n"
            "  --no-header         Skip header in both files\n"
            "  --no-node-header    Skip header in nodes file only\n"
            "  --no-edge-header    Skip header in edges file only\n"
            "  --weighted          Read third column as edge weight\n"
            "  --keep-loops        Keep self-loop edges\n",
            prog);
    exit(1);
}

int main(int argc, char **argv)
{
    ConvertOptions opts;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--no-header")
            opts.node_header = opts.edge_header = false;
        else if (arg == "--no-node-header")
            opts.node_header = false;
        else if (arg == "--no-edge-header")
            opts.edge_header = false;
        else if (arg == "--weighted")
            opts.weighted = false;
        else if (arg == "--keep-loops")
            opts.skip_loops = true;
        else if (arg.substr(0, 6) == "--sep=")
        {
            auto val = arg.substr(6);
            opts.sep = (val == "tab" || val == "\\t") ? '\t' : val[0];
        }
        else if (arg[0] == '-')
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
        }
        else
            pos.push_back(std::string(arg));
    }

    if (pos.size() != 3)
        usage(argv[0]);

    try
    {
        convert_graph(pos[0], pos[1], pos[2], opts);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
