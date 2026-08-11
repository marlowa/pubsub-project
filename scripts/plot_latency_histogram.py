#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import argparse
import sys
import os

def parse_args():
    parser = argparse.ArgumentParser(
        description="Scan latency histogram logs and generate performance plots in JPG format."
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
    parser.add_argument(
        "-q", "--quality",
        type=int,
        default=95,
        help="JPEG quality (1-100, default: 95)"
    )
    return parser.parse_args()

def plot_histograms(log_file, output_dir, quality):
    if not os.path.exists(log_file):
        print(f"Error: File '{log_file}' not found.")
        sys.exit(1)

    datasets = {}
    current_label = None
    data = []

    # Parse space-delimited buckets from the log file
    with open(log_file, 'r') as f:
        for line in f:
            line = line.strip()
            if line.startswith("# DATASET:"):
                if current_label and data:
                    datasets[current_label] = pd.DataFrame(data, columns=['bucket', 'count'])
                current_label = line.split(":")[1].strip()
                data = []
            elif line.startswith("#") or not line:
                continue
            else:
                parts = line.split()
                if len(parts) == 2:
                    try:
                        data.append([int(parts[0]), int(parts[1])])
                    except ValueError:
                        continue

        if current_label and data:
            datasets[current_label] = pd.DataFrame(data, columns=['bucket', 'count'])

    if not datasets:
        print("No valid # DATASET blocks found.")
        return

    for label, df in datasets.items():
        plt.figure(figsize=(12, 7))
        
        # Plotting latency buckets
        plt.bar(df['bucket'], df['count'], width=10, color='royalblue', edgecolor='navy', alpha=0.7)
        
        plt.title(f'Latency Distribution: {label}\n(Tuned Environment Simulation)')
        plt.xlabel('Latency (nanoseconds)')
        plt.ylabel('Frequency (Count - Log Scale)')
        
        plt.yscale('log')
        plt.grid(True, which="both", ls="-", alpha=0.2)

        # 1000ns threshold marker
        plt.axvline(x=1000, color='red', linestyle='--', label='Slow Path Threshold')
        plt.legend()
        
        safe_label = label.lower().replace(' ', '_')
        output_path = os.path.join(output_dir, f"{safe_label}_latency.jpg")
        
        # FIX: We avoid passing 'quality' directly to savefig.
        # We pass it strictly via pil_kwargs, which is the modern standard.
        # If your backend is exceptionally old, it will ignore it rather than crash.
        plt.savefig(
            output_path, 
            format='jpg', 
            pil_kwargs={'quality': quality}
        )
            
        print(f"Generated plot: {output_path} (Quality: {quality})")
        plt.close()

if __name__ == "__main__":
    args = parse_args()
    plot_histograms(args.filename, args.output_dir, args.quality)
