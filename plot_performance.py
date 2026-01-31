#!/usr/bin/env python3
import matplotlib.pyplot as plot
import argparse
import sys
import os

# ------------------------------------------------------------
# Command-line argument parsing
# ------------------------------------------------------------
def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Extract behavioural statistics from gtest output and generate JPG plots."
    )
    parser.add_argument("filename", help="Path to the log file containing BEHAVIOUR-STATS blocks")
    parser.add_argument("-o", "--output-dir", default=".", help="Directory where plots will be saved")
    parser.add_argument("-q", "--quality", type=int, default=95, help="JPEG quality (1-100)")
    return parser.parse_args()

# ------------------------------------------------------------
# Parse BEHAVIOUR-STATS datasets from the log file
# ------------------------------------------------------------
def parse_behaviour_statistics(log_file_path):
    datasets = []
    current_dataset = None

    with open(log_file_path, "r") as file:
        for line in file:
            line = line.strip()

            if line.startswith("# DATASET: BEHAVIOUR-STATS"):
                current_dataset = {}
                continue

            if line.startswith("# END-DATASET"):
                if current_dataset:
                    datasets.append(current_dataset)
                current_dataset = None
                continue

            if current_dataset is not None and line:
                parts = line.split()
                key = parts[0]

                if key == "per_pool_counts":
                    current_dataset[key] = list(map(int, parts[1:]))
                else:
                    current_dataset[key] = int(parts[1])

    return datasets

# ------------------------------------------------------------
# Save a plot with consistent formatting
# ------------------------------------------------------------
def save_plot(path, quality):
    plot.tight_layout()
    plot.savefig(path, format="jpg", pil_kwargs={"quality": quality})
    plot.close()

# ------------------------------------------------------------
# Generate all plots for each dataset
# ------------------------------------------------------------
def plot_datasets(datasets, output_dir, quality):
    for index, data in enumerate(datasets):

        # Derived metrics
        total_allocations = data["total_allocations"]
        slow_path_allocations = data["slow_path_allocations"]
        fast_path_allocations = data["fast_path_allocations"]

        slow_path_percentage = (
            (slow_path_allocations / total_allocations) * 100
            if total_allocations > 0 else 0.0
        )

        per_pool_sum = sum(data["per_pool_counts"])
        fallback_count = total_allocations - per_pool_sum
        fallback_percentage = (
            (fallback_count / total_allocations) * 100
            if total_allocations > 0 else 0.0
        )

        # ------------------------------------------------------------
        # Plot 1: Fast vs Slow Path
        # ------------------------------------------------------------
        plot.figure(figsize=(10, 6))
        plot.bar(
            ["fast_path", "slow_path"],
            [fast_path_allocations, slow_path_allocations],
            color=["green", "orange"]
        )
        plot.title(
            f"Fast Path vs Slow Path Allocations (slow_path_percentage={slow_path_percentage:.2f}%)"
        )
        plot.ylabel("Allocation Count")
        plot.grid(axis="y", alpha=0.3)
        save_plot(os.path.join(output_dir, f"behaviour_fast_slow_{index}.jpg"), quality)

        # ------------------------------------------------------------
        # Plot 2: Per-Pool Allocation Distribution
        # ------------------------------------------------------------
        if data["per_pool_counts"]:
            plot.figure(figsize=(12, 6))
            pool_indices = list(range(len(data["per_pool_counts"])))
            plot.bar(pool_indices, data["per_pool_counts"], color="royalblue")
            plot.title(
                f"Per-Pool Allocation Counts (fallback_percentage={fallback_percentage:.2f}%)"
            )
            plot.xlabel("Pool Index")
            plot.ylabel("Allocations")
            plot.xticks(pool_indices)
            plot.grid(axis="y", alpha=0.3)
            save_plot(os.path.join(output_dir, f"behaviour_per_pool_{index}.jpg"), quality)
        else:
            print(f"Warning: Missing or empty per_pool_counts in dataset {index}")

        # ------------------------------------------------------------
        # Plot 3: Total Allocations vs Expansion Events
        # ------------------------------------------------------------
        plot.figure(figsize=(10, 6))
        plot.bar(
            ["total_allocations", "expansion_events"],
            [data["total_allocations"], data["expansion_events"]],
            color=["steelblue", "crimson"]
        )
        plot.title("Total Allocations vs Expansion Events")
        plot.ylabel("Count")
        plot.grid(axis="y", alpha=0.3)
        save_plot(os.path.join(output_dir, f"behaviour_expansion_{index}.jpg"), quality)

        # ------------------------------------------------------------
        # Plot 4: Summary Metrics (slow %, fallback %, expansions)
        # ------------------------------------------------------------
        plot.figure(figsize=(10, 6))
        labels = ["slow_path_%", "fallback_%", "expansion_events"]
        values = [slow_path_percentage, fallback_percentage, data["expansion_events"]]
        colours = ["orange", "purple", "crimson"]

        plot.bar(labels, values, color=colours)
        plot.title("Allocator Behaviour Summary Metrics")
        plot.ylabel("Value")
        plot.grid(axis="y", alpha=0.3)
        save_plot(os.path.join(output_dir, f"behaviour_summary_{index}.jpg"), quality)

        print(f"Generated plots for dataset {index}")

# ------------------------------------------------------------
# Entry point
# ------------------------------------------------------------
if __name__ == "__main__":
    args = parse_arguments()
    datasets = parse_behaviour_statistics(args.filename)

    if not datasets:
        print("No BEHAVIOUR-STATS datasets found.")
        sys.exit(1)

    plot_datasets(datasets, args.output_dir, args.quality)

