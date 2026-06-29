# ---
# title: "Compression Sizes for Long Term Graph Storage"
# output:
#   html_document
# ---

# ```r

library(dplyr)
library(tidyverse)

results <- read.csv("results.csv") |> filter(graph == "abm14")
results |> head()

lapply(results |> select(-"bytes"), unique)

results |>
  arrange(bytes) |>
  head()

# 2.8GB
results |>
  filter(sorted == 0, compression == "none", encoding == "dictionary", repr == "edgelist")
# 2.3GB
results |>
  filter(sorted == 0, compression == "zstd:19", encoding == "dictionary", repr == "edgelist")

# edgelist < csr; delta + zstd best; sorted >> unsorted
results |>
  group_by(repr, sorted) |>
  slice_min(bytes)

results |>
  filter(compression == "zstd:19") |>
  group_by(encoding, sorted) |>
  slice_min(bytes)

results |>
  filter(compression == "none") |>
  group_by(encoding, sorted) |>
  slice_min(bytes)


# ```
