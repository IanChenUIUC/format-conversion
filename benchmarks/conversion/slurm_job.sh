#!/bin/bash
# SLURM array template for the cross-library conversion benchmark.
#
# One array task == one jobs.csv data row == one (comparison, tool, conversion,
# threads, graph) point. Each task runs both trials (warmup + measure) on the
# same node via run_job.py, then writes results/<N>.csv.
#
# Generate jobs.csv and the matching --array range first:
#     python gen_jobs.py -o jobs.csv         # prints the sbatch line to use
#     sbatch --array=1-<N> slurm_job.sh
# Then aggregate once all tasks finish:
#     python aggregate.py
#
# EDIT the #SBATCH headers below for your cluster (partition, account, time,
# and especially --cpus-per-task to match the largest threads level in config).

#SBATCH --job-name=fmtconv-bench
#SBATCH --output=slurm-%A_%a.out
#SBATCH --error=slurm-%A_%a.err
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --time=02:00:00
##SBATCH --partition=PARTITION   # uncomment + set
##SBATCH --account=ACCOUNT       # uncomment + set
##SBATCH --exclusive             # recommended: isolate timing from co-tenants

set -euo pipefail

cd "$(dirname "$0")"

# Activate the project environment that has format_conversion + adapters' deps.
# Adjust to your cluster (module load / conda activate / source .venv/bin/activate).
if [[ -n "${BENCH_VENV:-}" ]]; then
    # shellcheck disable=SC1091
    source "${BENCH_VENV}/bin/activate"
fi

# NUMA placement for large random-access arrays (see project README/DESIGN.md).
NUMACTL=""
if command -v numactl >/dev/null 2>&1; then
    NUMACTL="numactl --interleave=all"
fi

LINE="${SLURM_ARRAY_TASK_ID:?set via --array, or export for a manual run}"

exec ${NUMACTL} python run_job.py --job-line "${LINE}"
