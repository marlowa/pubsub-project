#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import argparse
import sys
import os

def parse_args():
    parser = argparse.ArgumentParser(
        description="Scan latency histogram logs and generate performance plots."
    )
    parser.add_argument(
        "filename", 
        help="Path to the log file containing # DATASET blocks"
    )
    parser.add_argument(
        "-o", "--output-dir", 
        default=".", 
        help="Directory to save generated plots (default: current)"
    )
    return parser.parse_args()

def plot_histograms(log_file, output_dir):
    if not os.path.exists(log_file):
        print(f"Error: File '{log_file}' not found.")
        sys.exit(1)

    datasets = {}
    current_label = None
    data = []

    # Parse the space-delimited format, ignoring comments and metadata
    with open(log_file, 'r') as f:
        for line in f:
            line = line.strip()
            if line.startswith("# DATASET:"):
                # Save previous dataset if exists
                if current_label and data:
                    datasets[current_label] = pd.DataFrame(data, columns=['bucket', 'count'])
                
                current_label = line.split(":")[1].strip()
                data = []
            elif line.startswith("#") or not line:
                continue
            else:
                # Handle space-delimited values
                parts = line.split()
                if len(parts) == 2:
                    try:
                        data.append([int(parts[0]), int(parts[1])])
                    except ValueError:
                        continue

        # Catch the final dataset in the file
        if current_label and data:
            datasets[current_label] = pd.DataFrame(data, columns=['bucket', 'count'])

    if not datasets:
        print("No valid # DATASET blocks found in the provided file.")
        return

    # Generate Plots
    for label, df in datasets.items():
        plt.figure(figsize=(12, 7))
        
        # Use a bar chart to represent the buckets
        plt.bar(df['bucket'], df['count'], width=10, color='royalblue', edgecolor='navy', alpha=0.7)
        
        plt.title(f'Latency Distribution: {label}\n(Tuned Environment Simulation)')
        plt.xlabel('Latency (nanoseconds)')
        plt.ylabel('Frequency (Count - Log Scale)')
        
        # Use log scale to ensure 1000ns+ outliers are visible alongside the primary peak
        plt.yscale('log')
        plt.grid(True, which="both", ls="-", alpha=0.2)

        # Highlight the 1000ns threshold which marks the "Safety Valve" activation
        plt.axvline(x=1000, color='red', linestyle='--', label='Slow Path Threshold')
        plt.legend()
        
        safe_label = label.lower().replace(' ', '_')
        output_path = os.path.join(output_dir, f"{safe_label}_latency.png")
        
        plt.savefig(output_path)
        print(f"Generated plot: {output_path}")
        plt.close()

if __name__ == "__main__":
    args = parse_args()
    plot_histograms(args.filename, args.output_dir)
