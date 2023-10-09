import regex as re
import numpy as np
from statistics import mean, stdev
import matplotlib
import random
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
#from bento_bench import bar_plot, parse_elapsed_secs, get_avg_from_table_str, set_size
import pandas as pd
import json
import os
import seaborn as sns

fio_iops_avg_regex = "(?<=^\s*iops\s*: min=\s*\d*, max=\s*\d*, avg=\s*)\d+\.\d*"
fio_iops_stdev_regex = "(?<=^\s*iops\s*: min=\s*\d*, max=\s*\d*, avg=\s*\d*\.*\d*, stdev=\s*)\d+\.\d*"

# Some are in KiB, some in MiB....
fio_bw_kib_avg_regex = "(?<=^\s*bw \(  KiB\/s\): min=\s*\d*, max=\s*\d*, per=\s*\d*\.*\d*%, avg=\s*)\d+\.\d*"
fio_bw_mib_avg_regex = "(?<=^\s*bw \(  MiB\/s\): min=\s*\d*, max=\s*\d*, per=\s*\d*\.*\d*%, avg=\s*)\d+\.\d*"
fio_bw_mib_avg_regex_alt = "(?<=BW=)\d+\.*\d*"
fio_bw_kib_stdev_regex = "(?<=^\s*bw \(  KiB\/s\): min=\s*\d*, max=\s*\d*, per=\s*\d*\.*\d*%, avg=\s*\d*\.*\d*, stdev=\s*)\d+\.\d*"
fio_bw_mib_stdev_regex = "(?<=^\s*bw \(  MiB\/s\): min=\s*\d*, max=\s*\d*, per=\s*\d*\.*\d*%, avg=\s*\d*\.*\d*, stdev=\s*)\d+\.\d*"

fio_clat_usec_avg_regex = "(?<=^\s*clat \(usec\): min=\d+, max=\d+\.*\d*k*M*, avg=\s*)\d+\.\d*"
fio_clat_nsec_avg_regex = "(?<=^\s*clat \(nsec\): min=\d+, max=\d+\.*\d*k*M*, avg=\s*)\d+\.\d*"
fio_clat_msec_avg_regex = "(?<=^\s*clat \(msec\): min=\d+, max=\d+\.*\d*k*M*, avg=\s*)\d+\.\d*"
fio_clat_usec_stdev_regex = "(?<=^\s*clat \(usec\): min=\d+, max=\d+\.*\d*k*M*, avg=\d+\.*\d*, stdev=\s*)\d+\.\d*"
fio_clat_nsec_stdev_regex = "(?<=^\s*clat \(nsec\): min=\d+, max=\d+\.*\d*k*M*, avg=\d+\.*\d*, stdev=\s*)\d+\.\d*"
fio_clat_msec_stdev_regex = "(?<=^\s*clat \(msec\): min=\d+, max=\d+\.*\d*k*M*, avg=\d+\.*\d*, stdev=\s*)\d+\.\d*"

fio_log_regex = "(?<=^\d*, )\d*"

redis_regex = "\d*.\d*(?= requests per second)"
rocksdb_regex = "\d*(?= ops\/sec )"
filebench_regex = "\d*.\d*(?= ops/s)"

fio_cols = ["conf", "RW", "BS", "QD", "P", "IOPS_avg", "IOPS_stdev", "clat_avg", "clat_stdev", "bw_avg", "bw_stdev"]
metadata_cols = ["conf", "workload", "type", "avg", "stdev"]

def parse_fio(RW, BS, QD, P, conf, folder):
    df = pd.DataFrame(columns=fio_cols)
    
    for rw in RW:
        for bs in BS:
            for qd in QD:
                for p in P:
                    path = "fio_" + rw + "_" + bs + "_" + qd + "_" + p + ".out"
                    f = open(folder + path)
                    f_buf = f.read()

                    # IOPS
                    avg_matches = re.findall(fio_iops_avg_regex, f_buf, re.MULTILINE)
                    stdev_matches = re.findall(fio_iops_stdev_regex, f_buf, re.MULTILINE)
                    row = {
                        "conf": conf,
                        "RW": rw,
                        "BS": bs,
                        "QD": qd,
                        "P": p,
                        "IOPS_avg": np.array(avg_matches).astype(float)[0],
                        "IOPS_stdev": np.array(stdev_matches).astype(float)[0]
                    }

                    # Completion latency
                    avg_matches = np.array(re.findall(fio_clat_usec_avg_regex, f_buf, re.MULTILINE)).astype(float)
                    if (len(avg_matches) == 0):
                        avg_matches = np.array(re.findall(fio_clat_nsec_avg_regex, f_buf, re.MULTILINE)).astype(float)
                        avg_matches = avg_matches / 1000.0 # convert to usec
                    if (len(avg_matches) == 0):
                        avg_matches = np.array(re.findall(fio_clat_msec_avg_regex, f_buf, re.MULTILINE)).astype(float)
                        avg_matches = avg_matches * 1000.0 # convert to usec

                    stdev_matches = np.array(re.findall(fio_clat_usec_stdev_regex, f_buf, re.MULTILINE)).astype(float)
                    if (len(stdev_matches) == 0):
                        stdev_matches = np.array(re.findall(fio_clat_nsec_stdev_regex, f_buf, re.MULTILINE)).astype(float)
                        stdev_matches = stdev_matches / 1000.0 # convert to usec
                    if (len(stdev_matches) == 0):
                        stdev_matches = np.array(re.findall(fio_clat_msec_stdev_regex, f_buf, re.MULTILINE)).astype(float)
                        stdev_matches = stdev_matches * 1000.0 # convert to usec

                    row["clat_avg"] = np.array(avg_matches).astype(float)[0]
                    row["clat_stdev"] = np.array(stdev_matches).astype(float)[0]

                    # Bandwidth
                    avg_matches = np.array(re.findall(fio_bw_mib_avg_regex, f_buf, re.MULTILINE)).astype(float)
                    if (len(avg_matches) == 0):
                        avg_matches = np.array(re.findall(fio_bw_kib_avg_regex, f_buf, re.MULTILINE)).astype(float)
                        avg_matches = avg_matches / 1024.0 # convert to MiB
                    if int(p) > 2:
                        avg_matches = np.array(re.findall(fio_bw_mib_avg_regex_alt, f_buf, re.MULTILINE)).astype(float)
                        

                    stdev_matches = np.array(re.findall(fio_bw_mib_stdev_regex, f_buf, re.MULTILINE)).astype(float)
                    if (len(stdev_matches) == 0):
                        stdev_matches = np.array(re.findall(fio_bw_kib_stdev_regex, f_buf, re.MULTILINE)).astype(float)
                        stdev_matches = stdev_matches / 1024.0 # convert to MiB

                    row["bw_avg"] = np.array(avg_matches).astype(float)[0]
                    row["bw_stdev"] = np.array(stdev_matches).astype(float)[0]

                    df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
                
    return df

def parse_fio_mt(RW, BS, QD, P, T, MT, conf, folder):
    df = pd.DataFrame(columns=fio_cols)

    for rw in RW:
        for bs in BS:
            for qd in QD:
                for p in P:
                    for t in T:
                        for mt in MT:
                            IOPS_avg = []
                            bw_avg = []
                            clat_avg = []
                            for mtt in range(0, int(mt)):
                                path = "/T" + t + "/MT_" + mt + "/fio_" + rw + "_" + bs + "_" + qd + "_" + p + "_" + str(mtt) + ".out"
                                f = open(folder + path)
                                f_buf = f.read()
            
                                # IOPS
                                avg_matches = re.findall(fio_iops_avg_regex, f_buf, re.MULTILINE)
                                if len(avg_matches) == 0:
                                    continue
                                IOPS_avg.append(np.array(avg_matches).astype(float)[0])
            
                                # Completion latency
                                avg_matches = np.array(re.findall(fio_clat_usec_avg_regex, f_buf, re.MULTILINE)).astype(float)
                                if (len(avg_matches) == 0):
                                    avg_matches = np.array(re.findall(fio_clat_nsec_avg_regex, f_buf, re.MULTILINE)).astype(float)
                                    avg_matches = avg_matches / 1000.0 # convert to usec
                                if (len(avg_matches) == 0):
                                    avg_matches = np.array(re.findall(fio_clat_msec_avg_regex, f_buf, re.MULTILINE)).astype(float)
                                    avg_matches = avg_matches * 1000.0 # convert to usec
            
                                clat_avg.append(np.array(avg_matches).astype(float)[0])
            
                                # Bandwidth
                                avg_matches = np.array(re.findall(fio_bw_mib_avg_regex, f_buf, re.MULTILINE)).astype(float)
                                if (len(avg_matches) == 0):
                                    avg_matches = np.array(re.findall(fio_bw_kib_avg_regex, f_buf, re.MULTILINE)).astype(float)
                                    avg_matches = avg_matches / 1024.0 # convert to MiB
                                if int(p) > 2:
                                    avg_matches = np.array(re.findall(fio_bw_mib_avg_regex_alt, f_buf, re.MULTILINE)).astype(float)
            
                                bw_avg.append(np.array(avg_matches).astype(float)[0])
    
                            row = {
                                "conf": conf,
                                "RW": rw,
                                "BS": bs,
                                "QD": qd,
                                "P": p,
                                "T": t,
                                "MT": mt,
                                "IOPS_avg": sum(IOPS_avg),
                                "IOPS_stdev": stdev(IOPS_avg) if len(IOPS_avg) > 1 else 0,
                                "bw_avg": sum(bw_avg),
                                "bw_stdev": stdev(bw_avg) if len(bw_avg) > 1 else 0,
                                "clat_avg": mean(clat_avg),
                                "clat_stdev": stdev(clat_avg) if len(clat_avg) > 1 else 0
                            }
                            df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
                
    return df

def parse_metadata(conf, folder):
    df = pd.DataFrame(columns=metadata_cols)
    tmp_set = []
    tmp_get = []
    dir = folder + "/redis"
    for f in os.listdir(dir):
        if f == ".ipynb_checkpoints": continue
        i = int(f.split("_")[1])
        fr = open(dir + "/" + f, 'r')
        t = fr.read()
        fr.close()
        matches = re.findall(redis_regex, t)
        tmp_set.append(float(matches[0]) * 0.001) # convert
        tmp_get.append(float(matches[1]) * 0.001) # convert
    
    row = {
        "conf": conf,
        "workload": "redis",
        "type": "get",
        "avg": mean(tmp_get),
        "stdev": stdev(tmp_get),
    }
    df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
    row = {
        "conf": conf,
        "workload": "redis",
        "type": "set",
        "avg": mean(tmp_set),
        "stdev": stdev(tmp_set),
    }
    df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
    
    tmp_fillrandom = []
    tmp_readrandom = []
    dir = folder + "/rocksdb"
    for f in os.listdir(dir):
        if f == ".ipynb_checkpoints": continue
        i = int(f.split("_")[1])
        fr = open(dir + "/" + f, 'r')
        t = fr.read()
        fr.close()
        matches = re.findall(rocksdb_regex, t)
        tmp_fillrandom.append(float(matches[0]) * 0.001)
        tmp_readrandom.append(float(matches[2]) * 0.001)
    
    row = {
        "conf": conf,
        "workload": "rocksdb",
        "type": "fillrandom",
        "avg": mean(tmp_fillrandom),
        "stdev": stdev(tmp_fillrandom),
    }
    df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
    row = {
        "conf": conf,
        "workload": "rocksdb",
        "type": "readrandom",
        "avg": mean(tmp_readrandom),
        "stdev": stdev(tmp_readrandom),
    }
    df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)

    fb_types = ['fileserver', 'varmail', 'webserver']
    for type in fb_types:
        tmp = []
        
        dir = folder + "/filebench"
        for f in os.listdir(dir):
            if f == ".ipynb_checkpoints": continue
            if f.split("_")[0] != type:
                continue
            if conf == "DPFS-RVFS-Ceph-T1-polling" and f == "fileserver_2":
                continue # This one bugged out, just skip it
                
            i = int(f.split("_")[1])
            fr = open(dir + "/" + f, 'r')
            t = fr.read()
            fr.close()
            matches = re.findall(filebench_regex, t)
            tmp.append(float(matches[0]) * 0.001)
        
        row = {
            "conf": conf,
            "workload": "filebench",
            "type": type,
            "avg": mean(tmp),
            "stdev": stdev(tmp),
        }
        df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
    
    return df

# Throughput
def plot_tp(df, confs, BS_list, P_list, conf_colormap, BS_colormap, P_colormap, GiB, output, legend_none=False, legend_loc='best', legend_ncols=1):
    fig, ax = plt.subplots()

    x = df.loc[(df['RW'] == 'randread') & (df['BS'] == BS_list[0]) & (df['P'] == P_list[0]) & (df['conf'] == list(confs.keys())[0]), 'QD']
    for (conf, conf_name) in confs.items():
        for bs in BS_list:
            for p in P_list:
                tp_data = df.loc[(df['BS'] == bs) & (df['conf'] == conf) & (df['P'] == p)]
                if GiB:
                    tp_data['bw_avg'] = tp_data['bw_avg'] / 1024.0
                    tp_data['bw_stdev'] = tp_data['bw_stdev'] / 1024.0
                    
    
                color = None
                if BS_colormap is not None:
                    color = BS_colormap[bs]
                elif conf_colormap is not None:
                    color = conf_colormap[conf]
                elif P_colormap is not None:
                    color = P_colormap[p]
                ax.errorbar(x, tp_data.loc[(tp_data['RW'] == 'randread'), 'bw_avg'], tp_data.loc[(tp_data['RW'] == 'randread'), 'bw_stdev'],
                            markersize=4, linestyle='-', label=conf_name + "read " + bs, capsize=5, capthick=1, color=color)
                ax.errorbar(x, tp_data.loc[(tp_data['RW'] == 'randwrite'), 'bw_avg'], tp_data.loc[(tp_data['RW'] == 'randwrite'), 'bw_stdev'],
                            markersize=4, linestyle='--', label=conf_name + "write " + bs, capsize=5, capthick=1, color=color)

    fig.tight_layout()
    ax.grid(which='major', linestyle='dashed', linewidth='1')
    ax.set_axisbelow(True)
    ax.legend(loc='best')
    ax.set_ylim(bottom=0)
    if GiB:
        ax.set_ylabel("Throughput (GiB/s)")
    else:
        ax.set_ylabel("Throughput (MiB/s)")
    ax.set_xlabel("I/O depth")
    
    legend_dict = {'Read': Line2D([0], [0], color='black', linestyle='-', lw=2),
                   'Write': Line2D([0], [0], color='black', linestyle='--', lw=2)}
    if conf_colormap is not None:
        for (k, v) in conf_colormap.items():
            legend_dict[confs[k]] = Line2D([0], [0], color=v, lw=2)
    if BS_colormap is not None:
        for (k, v) in BS_colormap.items():
            legend_dict[k] = Line2D([0], [0], color=v, lw=2)
    if P_colormap is not None:
        for (k, v) in P_colormap.items():
            legend_dict["P="+k] = Line2D([0], [0], color=v, lw=2)
    
    legend_names = [w.replace('k', ' KiB') for w in legend_dict.keys()]
    ax.legend(legend_dict.values(), legend_names, loc=legend_loc, ncols=legend_ncols)
    if legend_none:
        ax.get_legend().remove()

    plt.savefig(output, bbox_inches="tight")

def plot_tp_mt(df, confs, QD, P, T, BS_list, conf_colormap, BS_colormap, GiB, output, legend_outside=False, legend_none=False):
    fig, ax = plt.subplots()

    x = df.loc[(df['RW'] == 'randread') & (df['BS'] == BS_list[0]) & (df['P'] == P)
               & (df['conf'] == list(confs.keys())[0]) & (df['conf'] == list(confs.keys())[0]) & (df['QD'] == QD) & (df['T'] == T), 'MT']
    for (conf, conf_name) in confs.items():
        for bs in BS_list:
            tp_data = df.loc[(df['BS'] == bs) & (df['conf'] == conf) & (df['P'] == P) & (df['QD'] == QD) & (df['T'] == T)]
            if GiB:
                tp_data['bw_avg'] = tp_data['bw_avg'] / 1024.0
                tp_data['bw_stdev'] = tp_data['bw_stdev'] / 1024.0
                
    
            color = None
            if BS_colormap is not None:
                color = BS_colormap[bs]
            elif conf_colormap is not None:
                color = conf_colormap[conf]
            ax.errorbar(x, tp_data.loc[(tp_data['RW'] == 'randread'), 'bw_avg'], tp_data.loc[(tp_data['RW'] == 'randread'), 'bw_stdev'],
                        markersize=4, linestyle='-', label=conf_name + "read " + bs, capsize=5, capthick=1, color=color)
            ax.errorbar(x, tp_data.loc[(tp_data['RW'] == 'randwrite'), 'bw_avg'], tp_data.loc[(tp_data['RW'] == 'randwrite'), 'bw_stdev'],
                        markersize=4, linestyle='--', label=conf_name + "write " + bs, capsize=5, capthick=1, color=color)

    fig.tight_layout()
    ax.grid(which='major', linestyle='dashed', linewidth='1')
    ax.set_axisbelow(True)
    ax.legend(loc='best')
    ax.set_ylim(bottom=0)
    if GiB:
        ax.set_ylabel("Throughput (GiB/s)")
    else:
        ax.set_ylabel("Throughput (MiB/s)")
    ax.set_xlabel("Number of tenants")
    
    legend_dict = {'Read': Line2D([0], [0], color='black', linestyle='-', lw=2),
                   'Write': Line2D([0], [0], color='black', linestyle='--', lw=2)}
    if conf_colormap is not None:
        for (k, v) in conf_colormap.items():
            legend_dict[confs[k]] = Line2D([0], [0], color=v, lw=2)
    if BS_colormap is not None:
        for (k, v) in BS_colormap.items():
            legend_dict[k] = Line2D([0], [0], color=v, lw=2)
    
    legend_names = [w.replace('k', ' KiB') for w in legend_dict.keys()]

    if legend_outside:
        # Shrink current axis by 20%
        box = ax.get_position()
        ax.set_position([box.x0, box.y0, box.width * 0.8, box.height])
        
        # Put a legend to the right of the current axis
        ax.legend(legend_dict.values(), legend_names, loc='center left', bbox_to_anchor=(1, 0.5))
    else:
        ax.legend(legend_dict.values(), legend_names, loc='best')

    if legend_none:
        ax.get_legend().remove()

    plt.savefig(output, bbox_inches="tight")

fio_clat_cols = ["conf", "RW", "BS", "QD", "P", "clat"]

def parse_fio_clat(RW, BS, QD, P, conf, folder):
    df = pd.DataFrame(columns=fio_clat_cols)
    
    for rw in RW:
        for bs in BS:
            for qd in QD:
                for p in P:
                    path = "fio_" + rw + "_" + bs + "_" + qd + "_" + p + "_clat.1.log"
                    f = open(folder + path)
                    matches = re.findall(fio_log_regex, f.read(), re.MULTILINE)
                    print(matches)

                    row = {
                        "conf": conf,
                        "RW": rw,
                        "BS": bs,
                        "QD": qd,
                        "P": p,
                        "clat": np.array(matches).astype(int) * 0.000001 # ns to ms
                    }
                    
                    df = pd.concat((df, pd.DataFrame([row])), ignore_index=True)
    return df

# CDF
def plot_cdf(df, confs, RW_list, BS, QD, P, conf_colormap, output):
    fig, ax = plt.subplots()

    avg_y = 0.48
    for (conf, conf_name) in confs.items():
            data = df.loc[(df['BS'] == BS) & (df['conf'] == conf) & (df['P'] == P) & (df['QD'] == QD)]
                
            c = None
            if conf_colormap is not None:
                c = conf_colormap[conf]
            for rw in RW_list:
                d = data.loc[(data['RW'] == rw), 'clat'].iloc[0]
            
                x = np.sort(d)
                y = np.arange(len(d)) / len(d)
                style = '-' if rw == 'randread' else '--'
                ax.plot(x, y, color=c, label=conf_name + " " + rw, linestyle=style)

                print("99.9th percentile of " + conf_name + " " + rw + " = " + str(x[int(len(d) / 1000 * 999)]) + "ms")
                print("99.99th percentile of " + conf_name + " " + rw + " = " + str(x[int(len(d) / 10000 * 9999)]) + "ms")
                print("99.999th percentile of " + conf_name + " " + rw + " = " + str(x[int(len(d) / 100000 * 99999)]) + "ms")

            avg = np.mean(data.loc[(), 'clat'].iloc[0])
            ax.scatter(avg, avg_y, color=c, marker='o', s=50, label='Avg ' + rw)
            avg_y = avg_y + 0.04

    fig.tight_layout()
    ax.set_xscale('log', base=2)
    ax.set_xlabel("Latency (ms)")
    ax.set_ylabel("Proportion")
    ax.grid(which='major', linestyle='dashed', linewidth='1')
    ax.set_axisbelow(True)
    ax.legend(loc='best')
    ax.set_yticks([0, 0.2, 0.4, 0.6, 0.8, 1.0])

    
    legend_dict = {'Read': Line2D([0], [0], color='black', linestyle='-', lw=2),
                   'Write': Line2D([0], [0], color='black', linestyle='--', lw=2)}
    if conf_colormap is not None:
        for (k, v) in conf_colormap.items():
            legend_dict[confs[k]] = Line2D([0], [0], color=v, lw=2)
    ax.legend(legend_dict.values(), legend_dict.keys(), loc='best')
    
    plt.savefig(output, bbox_inches="tight")

# def plot_bar(ax, data, l, output, yerr=None, colors=None, total_width=0.8, single_width=1):
def plot_bar(df, confs, workload, output, colors=None, total_width=0.8, single_width=1, ylim=None):
    fig, ax = plt.subplots()

    types = df.loc[(df['workload'] == workload)]['type'].unique()
    # Number of bars per group
    n_bars = len(types)

    # The width of a single bar
    bar_width = total_width / n_bars

    # List containing handles for the drawn bars, used for the legend
    bars = []

    for i, conf in enumerate(confs.keys()):
        # The offset in x direction of that bar
        x_offset = (i - n_bars / 2) * bar_width + bar_width / 2
        
        ydata = df.loc[(df['workload'] == workload) & (df['conf'] == conf)]['avg']
        yerr = df.loc[(df['workload'] == workload) & (df['conf'] == conf)]['stdev']
        for x, y in enumerate(ydata):
            color = colors[conf] if colors is not None else None
            bar = ax.bar(x + x_offset, y, yerr=yerr.iloc[x], width=bar_width * single_width, color=color)
            # Add a handle to a drawn bar, which we'll need for the legend
            if x == 0:
                bars.append(bar)
    
    ax.legend(bars, confs.values())

    
    plt.xticks(range(len(types)), types)
    if workload == "filebench" or workload == "rocksdb":
        plt.ylabel("Kops/s")
    elif workload == "redis":
        plt.ylabel("MB/s")
    if ylim is not None:
        plt.ylim(0, ylim)
    plt.savefig(output, bbox_inches="tight")