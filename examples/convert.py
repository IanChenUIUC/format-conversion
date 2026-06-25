import sys
import pathlib
import format_conversion as fmt

format = sys.argv[-4]
nodelist = pathlib.Path(sys.argv[-3])
edgelist = pathlib.Path(sys.argv[-2])
output = pathlib.Path(sys.argv[-1])

opt = fmt.ConvertOptions()
if edgelist.suffix == ".tsv":
    opt.sep = "\t"
    opt.edge_header = False
else:
    opt.sep = ","
    opt.edge_header = True

if format.lower() == "metis":
    opt.type = fmt.OutputType.METIS
elif format.lower() == "csr":
    opt.type = fmt.OutputType.CSR

output.mkdir(exist_ok=True)
fmt.convert(
    nodelist,
    edgelist,
    output,
    opt,
)
