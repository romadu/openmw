#!/usr/bin/env python3
"""
osg_stats.py is a script to analyze OpenSceneGraph log. It parses given file
and builds timeseries, histograms, plots, calculate statistics for a given
set of keys over given range of frames.
"""

import click
import collections
import matplotlib.pyplot
import numpy
import statistics
import sys
import termtables


@click.command()
@click.option('--print_keys', is_flag=True,
              help='Print a list of all present keys in the input file.')
@click.option('--timeseries', type=str, multiple=True,
              help='Show a graph for given metric over time.')
@click.option('--commulative_timeseries', type=str, multiple=True,
              help='Show a graph for commulative sum of a given metric over time.')
@click.option('--hist', type=str, multiple=True,
              help='Show a histogram for all values of given metric.')
@click.option('--hist_ratio', nargs=2, type=str, multiple=True,
              help='Show a histogram for a ratio of two given metric (first / second). '
                   'Format: --hist_ratio <first_metric> <second_metric>.')
@click.option('--stdev_hist', nargs=2, type=str, multiple=True,
              help='Show a histogram for a standard deviation of a given metric at given scale (number). '
                   'Format: --stdev_hist <metric> <scale>.')
@click.option('--plot', nargs=3, type=str, multiple=True,
              help='Show a 2D plot for relation between two metrix (first is axis x, second is y)'
                   'using one of aggregation functions (mean, median). For example show a relation '
                   'between Physics Actors and physics_time_taken. Format: --plot <x> <y> <function>.')
@click.option('--stats', type=str, multiple=True,
              help='Print table with stats for a given metric containing min, max, mean, median etc.')
@click.option('--timeseries_sum', is_flag=True,
              help='Add a graph to timeseries for a sum per frame of all given timeseries metrics.')
@click.option('--commulative_timeseries_sum', is_flag=True,
            help='Add a graph to timeseries for a sum per frame of all given commulative timeseries.')
@click.option('--stats_sum', is_flag=True,
              help='Add a row to stats table for a sum per frame of all given stats metrics.')
@click.option('--begin_frame', type=int, default=0,
              help='Start processing from this frame.')
@click.option('--end_frame', type=int, default=sys.maxsize,
              help='End processing at this frame.')
@click.argument('path', type=click.Path(), nargs=-1)
def main(print_keys, timeseries, hist, hist_ratio, stdev_hist, plot, stats,
         timeseries_sum, stats_sum, begin_frame, end_frame, path,
         commulative_timeseries, commulative_timeseries_sum):
    sources = {v: list(read_data(v)) for v in path} if path else {'stdin': list(read_data(None))}
    keys = collect_unique_keys(sources)
    frames = collect_per_frame(sources=sources, keys=keys, begin_frame=begin_frame, end_frame=end_frame)
    if print_keys:
        for v in keys:
            print(v)
    if timeseries:
        draw_timeseries(sources=frames, keys=timeseries, add_sum=timeseries_sum)
    if commulative_timeseries:
        draw_commulative_timeseries(sources=frames, keys=commulative_timeseries, add_sum=commulative_timeseries_sum)
    if hist:
        draw_hists(sources=frames, keys=hist)
    if hist_ratio:
        draw_hist_ratio(sources=frames, pairs=hist_ratio)
    if stdev_hist:
        draw_stdev_hists(sources=frames, stdev_hists=stdev_hist)
    if plot:
        draw_plots(sources=frames, plots=plot)
    if stats:
        print_stats(sources=frames, keys=stats, stats_sum=stats_sum)
    matplotlib.pyplot.show()


def read_data(path):
    with open(path) if path else sys.stdin as stream:
        frame = dict()
        camera = 0
        for line in stream:
            if line.startswith('Stats Viewer'):
                if frame:
                    camera = 0
                    yield frame
                _, _, key, value = line.split(' ')
                frame = {key: int(value)}
            elif line.startswith('Stats Camera'):
                camera += 1
            elif line.startswith('    '):
                key, value = line.strip().rsplit(maxsplit=1)
                if camera:
                    key = f'{key} Camera {camera}'
                frame[key] = to_number(value)


def collect_per_frame(sources, keys, begin_frame, end_frame):
    result = collections.defaultdict(lambda: collections.defaultdict(list))
    for name, frames in sources.items():
        for frame in frames:
            for key in keys:
                if key in frame:
                    result[name][key].append(frame[key])
                else:
                    result[name][key].append(None)
    for name, sources in result.items():
        for key, values in sources.items():
            result[name][key] = numpy.array(values[begin_frame:end_frame])
    return result


def collect_unique_keys(sources):
    result = set()
    for frames in sources.values():
        for frame in frames:
            for key in frame.keys():
                result.add(key)
    return sorted(result)


def draw_timeseries(sources, keys, add_sum):
    fig, ax = matplotlib.pyplot.subplots()
    for name, frames in sources.items():
        x = numpy.array(range(max(len(v) for k, v in frames.items() if k in keys)))
        for key in keys:
            print(key, name)
            ax.plot(x, frames[key], label=f'{key}:{name}')
        if add_sum:
            ax.plot(x, numpy.sum(list(frames[k] for k in keys), axis=0), label=f'sum:{name}')
    ax.grid(True)
    ax.legend()
    fig.canvas.set_window_title('timeseries')


def draw_commulative_timeseries(sources, keys, add_sum):
    fig, ax = matplotlib.pyplot.subplots()
    for name, frames in sources.items():
        x = numpy.array(range(max(len(v) for k, v in frames.items() if k in keys)))
        for key in keys:
            ax.plot(x, numpy.cumsum(frames[key]), label=f'{key}:{name}')
        if add_sum:
            ax.plot(x, numpy.cumsum(numpy.sum(list(frames[k] for k in keys), axis=0)), label=f'sum:{name}')
    ax.grid(True)
    ax.legend()
    fig.canvas.set_window_title('commulative_timeseries')


def draw_hists(sources, keys):
    fig, ax = matplotlib.pyplot.subplots()
    bins = numpy.linspace(
        start=min(min(min(v) for k, v in f.items() if k in keys) for f in sources.values()),
        stop=max(max(max(v) for k, v in f.items() if k in keys) for f in sources.values()),
        num=20,
    )
    for name, frames in sources.items():
        for key in keys:
            ax.hist(frames[key], bins=bins, label=f'{key}:{name}', alpha=1 / (len(keys) * len(sources)))
    ax.set_xticks(bins)
    ax.grid(True)
    ax.legend()
    fig.canvas.set_window_title('hists')


def draw_hist_ratio(sources, pairs):
    fig, ax = matplotlib.pyplot.subplots()
    bins = numpy.linspace(
        start=min(min(min(a / b for a, b in zip(f[a], f[b])) for a, b in pairs) for f in sources.values()),
        stop=max(max(max(a / b for a, b in zip(f[a], f[b])) for a, b in pairs) for f in sources.values()),
        num=20,
    )
    for name, frames in sources.items():
        for a, b in pairs:
            ax.hist(frames[a] / frames[b], bins=bins, label=f'{a} / {b}:{name}', alpha=1 / (len(pairs) * len(sources)))
    ax.set_xticks(bins)
    ax.grid(True)
    ax.legend()
    fig.canvas.set_window_title('hists_ratio')


def draw_stdev_hists(sources, stdev_hists):
    for key, scale in stdev_hists:
        scale = float(scale)
        fig, ax = matplotlib.pyplot.subplots()
        first_frames = next(v for v in sources.values())
        median = statistics.median(first_frames[key])
        stdev = statistics.stdev(first_frames[key])
        start = median - stdev / 2 * scale
        stop = median + stdev / 2 * scale
        bins = numpy.linspace(start=start, stop=stop, num=9)
        for name, frames in sources.items():
            values = [v for v in frames[key] if start <= v <= stop]
            ax.hist(values, bins=bins, label=f'{key}:{name}', alpha=1 / (len(stdev_hists) * len(sources)))
        ax.set_xticks(bins)
        ax.grid(True)
        ax.legend()
        fig.canvas.set_window_title('stdev_hists')


def draw_plots(sources, plots):
    fig, ax = matplotlib.pyplot.subplots()
    for name, frames in sources.items():
        for x_key, y_key, agg in plots:
            if agg is None:
                ax.plot(frames[x_key], frames[y_key], label=f'x={x_key}, y={y_key}:{name}')
            elif agg:
                agg_f = dict(
                    mean=statistics.mean,
                    median=statistics.median,
                )[agg]
                grouped = collections.defaultdict(list)
                for x, y in zip(frames[x_key], frames[y_key]):
                    grouped[x].append(y)
                aggregated = sorted((k, agg_f(v)) for k, v in grouped.items())
                ax.plot(
                    numpy.array([v[0] for v in aggregated]),
                    numpy.array([v[1] for v in aggregated]),
                    label=f'x={x_key}, y={y_key}, agg={agg}:{name}',
                )
    ax.grid(True)
    ax.legend()
    fig.canvas.set_window_title('plots')


def print_stats(sources, keys, stats_sum):
    stats = list()
    for name, frames in sources.items():
        for key in keys:
            stats.append(make_stats(source=name, key=key, values=filter_not_none(frames[key])))
        if stats_sum:
            stats.append(make_stats(source=name, key='sum', values=sum_multiple(frames, keys)))
    metrics = list(stats[0].keys())
    max_key_size = max(len(tuple(v.values())[0]) for v in stats)
    termtables.print(
        [list(v.values()) for v in stats],
        header=metrics,
        style=termtables.styles.markdown,
    )


def filter_not_none(values):
    return [v for v in values if v is not None]


def sum_multiple(frames, keys):
    result = collections.Counter()
    for key in keys:
        values = frames[key]
        for i, value in enumerate(values):
            if value is not None:
                result[i] += float(value)
    return numpy.array([result[k] for k in sorted(result.keys())])


def make_stats(source, key, values):
    return collections.OrderedDict(
        source=source,
        key=key,
        number=len(values),
        min=min(values),
        max=max(values),
        mean=statistics.mean(values),
        median=statistics.median(values),
        stdev=statistics.stdev(values),
        q95=numpy.quantile(values, 0.95),
    )


def to_number(value):
    try:
        return int(value)
    except ValueError:
        return float(value)

if __name__ == '__main__':
    main()
