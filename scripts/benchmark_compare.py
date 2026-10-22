"""Head-to-head comparison of two preserved benchmark directories.

Every rate is reported with a Clopper-Pearson 95% interval rather than as a bare
point estimate, because at these sample sizes the point estimate alone invites a
decision the data cannot support. Differences are tested with Fisher's exact
test on the underlying counts; durations, which are skewed and bounded below,
are tested with Mann-Whitney rather than by comparing means.

Throughput carries a bootstrap interval. A tasks-per-hour figure computed from a
median duration and a success rate is a ratio of two estimates, and quoting it
to one decimal place without an interval overstates what thirty runs can say.
"""
import os
import sys
import statistics as st

from scipy import stats

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from benchmark_metrics import summarise  # noqa: E402

RNG_SEED = 20260914


def clopper_pearson(k, n, alpha=0.05):
    """Exact binomial interval. Conservative, and correct at k = 0 and k = n,
    where the normal approximation collapses to a zero-width interval."""
    if n == 0:
        return (0.0, 1.0)
    lo = 0.0 if k == 0 else stats.beta.ppf(alpha / 2, k, n - k + 1)
    hi = 1.0 if k == n else stats.beta.ppf(1 - alpha / 2, k + 1, n - k)
    return (float(lo), float(hi))


def rate_line(name, k, n):
    lo, hi = clopper_pearson(k, n)
    return '%-26s %2d/%-3d = %5.1f%%  [%4.1f%%, %5.1f%%]' % (
        name, k, n, 100.0 * k / n, 100 * lo, 100 * hi)


def fisher(ka, na, kb, nb):
    table = [[ka, na - ka], [kb, nb - kb]]
    _, p = stats.fisher_exact(table, alternative='two-sided')
    return p


def bootstrap_clean_per_hour(rows, iterations=20000):
    """Resample whole runs, so the rate and the duration move together the way
    they do in reality: a set of runs that happens to be cleaner is also a set
    with its own timings, and treating the two as independent would narrow the
    interval by pretending away that coupling."""
    import random
    rng = random.Random(RNG_SEED)
    n = len(rows)
    out = []
    for _ in range(iterations):
        sample = [rows[rng.randrange(n)] for _ in range(n)]
        durations = [r['duration_s'] for r in sample if r['duration_s']]
        if not durations:
            continue
        median = st.median(durations)
        clean = sum(1 for r in sample if r['clean'])
        out.append(3600.0 / median * clean / n)
    out.sort()
    if not out:
        return (0.0, 0.0)
    return (out[int(0.025 * len(out))], out[int(0.975 * len(out))])


def main(argv):
    if len(argv) != 3:
        print('usage: benchmark_compare.py <dir-A> <dir-B>')
        return 1

    sets = []
    for directory in argv[1:]:
        summary, rows = summarise(directory)
        if not summary:
            print('%s: no logs' % directory)
            return 1
        sets.append((os.path.basename(directory.rstrip('\\/')), summary, rows))

    (la, sa, ra), (lb, sb, rb) = sets
    na, nb = sa['runs'], sb['runs']

    for label, s, rows in sets:
        n = s['runs']
        print('=== %s  (n = %d) ===' % (label, n))
        print('  ENGINEERING')
        print('   ', rate_line('verified-task rate', round(s['verified_task_rate'] * n), n))
        print('   ', rate_line('runs w/ unsupported claims', s['runs_with_unsupported_claims'], n))
        print('    %-26s %d of %d calls' %
              ('invented locations', s['invented_locations'], s['total_tool_calls']))
        print('    %-26s %d of %d calls' %
              ('invalid tool calls', s['invalid_calls'], s['total_tool_calls']))
        print('    %-26s %d' % ('criteria left unsettled', s['criteria_unsettled']))
        print('  PRESENTATION')
        print('   ', rate_line('clean-completion rate', round(s['clean_completion_rate'] * n), n))
        print('   ', rate_line('final emitted rate', round(s['final_emitted_rate'] * n), n))
        print('   ', rate_line('finalizer activation', s['finalizer_entered'], n))
        if s['finalizer_entered']:
            print('   ', rate_line('finalizer recovery',
                                   s['finalization_recovered'], s['finalizer_entered']))
        else:
            print('    %-26s n/a (never entered)' % 'finalizer recovery')
        print('    %-26s %d' % ('presentation failures', s['presentation_failed']))
        print('  COST AND THROUGHPUT')
        durations = [r['duration_s'] for r in rows if r['duration_s']]
        q1, q3 = st.quantiles(durations, n=4)[0], st.quantiles(durations, n=4)[2]
        print('    %-26s %d / %d' % ('median steps / tool calls',
                                     s['median_steps'], s['median_tool_calls']))
        print('    %-26s %d' % ('gate interventions', s['gate_interventions']))
        print('    %-26s %d' % ('repeated calls', s['repeated_calls']))
        print('    %-26s %.0fs  (IQR %.0f-%.0f)' %
              ('median duration', s['median_duration_s'], q1, q3))
        print('    %-26s %.1f tok/s' % ('median decode', s['median_decode_tok_s'] or 0))
        print('    %-26s %.0f tok/s' % ('median prompt', s['median_prompt_tok_s'] or 0))
        print('    %-26s %.1f' % ('verified tasks/hour', s['verified_tasks_per_hour']))
        lo, hi = bootstrap_clean_per_hour(rows)
        print('    %-26s %.1f  [%.1f, %.1f]' %
              ('clean tasks/hour', s['clean_tasks_per_hour'], lo, hi))
        print()

    print('=== head to head: %s vs %s ===' % (la, lb))
    ka = round(sa['verified_task_rate'] * na)
    kb = round(sb['verified_task_rate'] * nb)
    print('  verified-task rate    %5.1f%% vs %5.1f%%   Fisher p = %.4f' %
          (100.0 * ka / na, 100.0 * kb / nb, fisher(ka, na, kb, nb)))

    ka = round(sa['clean_completion_rate'] * na)
    kb = round(sb['clean_completion_rate'] * nb)
    print('  clean-completion rate %5.1f%% vs %5.1f%%   Fisher p = %.4f' %
          (100.0 * ka / na, 100.0 * kb / nb, fisher(ka, na, kb, nb)))

    ka, kb = sa['runs_with_unsupported_claims'], sb['runs_with_unsupported_claims']
    print('  runs w/ unsupported   %5.1f%% vs %5.1f%%   Fisher p = %.4f' %
          (100.0 * ka / na, 100.0 * kb / nb, fisher(ka, na, kb, nb)))

    # Per-call rates are tested against the call totals, not the run totals: an
    # invented path is an event per tool call, and dividing it by runs would
    # reward whichever model happened to make more calls.
    ca, cb = sa['total_tool_calls'], sb['total_tool_calls']
    print('  invented locations    %d/%d vs %d/%d          Fisher p = %.4f' %
          (sa['invented_locations'], ca, sb['invented_locations'], cb,
           fisher(sa['invented_locations'], ca, sb['invented_locations'], cb)))
    print('  invalid tool calls    %d/%d vs %d/%d          Fisher p = %.4f' %
          (sa['invalid_calls'], ca, sb['invalid_calls'], cb,
           fisher(sa['invalid_calls'], ca, sb['invalid_calls'], cb)))

    da = [r['duration_s'] for r in ra if r['duration_s']]
    db = [r['duration_s'] for r in rb if r['duration_s']]
    _, p = stats.mannwhitneyu(da, db, alternative='two-sided')
    print('  duration              %.0fs vs %.0fs median    Mann-Whitney p = %.4f' %
          (st.median(da), st.median(db), p))

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
