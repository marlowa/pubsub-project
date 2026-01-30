#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import argparse
import sys
import os

def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract behavioural statistics from gtest output and generate JPG plots."
    )
    parser.add_argument("filename", help="Path to the log file containing BEHAVIOUR-STATS blocks")
    parser.add_argument("-o", "--output-dir", default=".", help="Directory to save plots")
    parser.add_argument("-q", "--quality", type=int, default=95, help="JPEG quality (1-100)")
    return parser.parse_args()

def parse_behaviour_stats(log_file):
    datasets = []
    current = None

    with open(log_file, "r") as f:
        for line in f:
            line = line.strip()

            if line.startswith("# DATASET: BEHAVIOUR-STATS"):
                current = {}
                continue

            if line.startswith("# END-DATASET"):
                if current:
                    datasets.append(current)
                current = None
                continue

            if current is not None and line:
                parts = line.split()
                key = parts[0]

                if key == "per_pool_counts":
                    # Parse space-delimited list of integers
                    current[key] = list(map(int, parts[1:]))
                else:
                    current[key] = int(parts[1])

    return datasets

def plot_datasets(datasets, output_dir, quality):
    for idx, data in enumerate(datasets):
        df = pd.DataFrame([data])

        # -------------------------------
        # Plot 1: Fast vs Slow Path
        # -------------------------------
        plt.figure(figsize=(10, 6))
        plt.bar(["fast", "slow"],
                [data["fast_path_allocations"], data["slow_path_allocations"]],
                color=["green", "orange"])
        plt.title("Fast vs Slow Path Allocations")
        plt.ylabel("Count")
        plt.grid(axis="y", alpha=0.3)
        plt.savefig(os.path.join(output_dir, f"behaviour_fast_slow_{idx}.jpg"),
                    format="jpg", pil_kwargs={"quality": quality})
        plt.close()

        # -------------------------------
        # Plot 2: Per-Pool Allocation Distribution
        # -------------------------------
        if "per_pool_counts" in data and len(data["per_pool_counts"]) > 0:
            plt.figure(figsize=(12, 6))

            pool_indices = list(range(len(data["per_pool_counts"])))
            plt.bar(pool_indices, data["per_pool_counts"], color="royalblue")

            plt.title("Per-Pool Allocation Counts")
            plt.xlabel("Pool Index")
            plt.ylabel("Allocations")

            # Explicitly set ticks to avoid negative axis behaviour
            plt.xticks(pool_indices)

            plt.grid(axis="y", alpha=0.3)
            plt.savefig(os.path.join(output_dir, f"behaviour_per_pool_{idx}.jpg"),
                        format="jpg", pil_kwargs={"quality": quality})
            plt.close()
        else:
            print(f"Warning: Missing or empty per_pool_counts in dataset {idx}")

        # -------------------------------
        # Plot 3: Total Allocations vs Expansion Events
        # -------------------------------
        plt.figure(figsize=(10, 6))
        plt.bar(["total_allocations", "expansion_events"],
                [data["total_allocations"], data["expansion_events"]],
                color=["steelblue", "crimson"])
        plt.title("Total Allocations vs Expansion Events")
        plt.grid(axis="y", alpha=0.3)
        plt.savefig(os.path.join(output_dir, f"behaviour_expansion_{idx}.jpg"),
                    format="jpg", pil_kwargs={"quality": quality})
        plt.close()

        print(f"Generated plots for dataset {idx}")

if __name__ == "__main__":
    args = parse_args()
    datasets = parse_behaviour_stats(args.filename)

    if not datasets:
        print("No BEHAVIOUR-STATS datasets found.")
        sys.exit(1)

    plot_datasets(datasets, args.output_dir, args.quality)
