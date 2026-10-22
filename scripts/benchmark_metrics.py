"""Extract the metric set from a preserved benchmark directory.

Reads only what the logs state. Anything absent is reported as absent rather
than defaulted to zero, because a missing field that silently becomes 0 is how a
measurement quietly turns into a wrong claim.

Engineering and presentation are reported separately throughout: a run whose
objective is verified and whose summary never arrived is not failed work.
"""
import io
import os
import re
import sys
import json
import glob
import statistics as st

# Locations invented by the model rather than resolved from the workspace. These
# are counted from the tool arguments directly: matching on error text misses
# them entirely, because an invented path under /workspace resolves to a real
# subdirectory and is then created rather than refused.
BOGUS = re.compile(r'/workspace/dumps|/workspace/claude|"(?:path|working_directory)":"scratchpad/')

INVALID = re.compile(r'invalid command shape|conflicting command|states nothing to check|'
                     r'must be a number|arrived damaged|artifact id', re.I)

# Generation rates, one line per model call.
RATE = re.compile(r'([\d.]+) tok/s decode\s+([\d.]+) tok/s prompt')

# Two different nudges share the "[no progress]" prefix and mean different
# things. The first is the loop pushing a stalled turn along; the second is the
# finalization phase being entered on work that is already verified. Counting
# them together would report the feature as a failure symptom.
FINALIZER_ENTERED = re.compile(r'\[no progress\] The work is verified; asking for the final answer\.')
STALL_NUDGE = re.compile(r'\[no progress\] (?!The work is verified)')

UNSETTLED = re.compile(r'acceptance criteria still unsettled: (.+)')


def strip_ansi(s):
    return re.sub(r'\x1b\[[0-9;]*m', '', s)


def field(text, name):
    m = re.search(r'^  ' + name + r'\s+(.+)$', text, re.M)
    return m.group(1).strip() if m else None


def duration_seconds(value):
    if not value:
        return None
    m = re.match(r'(?:(\d+)m)?([\d.]+)s', value.replace(' ', ''))
    if not m:
        return None
    return (int(m.group(1)) if m.group(1) else 0) * 60 + float(m.group(2))


def parse(path):
    text = strip_ansi(io.open(path, encoding='utf-8', errors='replace').read())

    claims = []
    i = text.find('Claims the evidence')
    if i > 0:
        j = text.find('Files changed', i)
        claims = [c.strip('- ').strip() for c in text[i:j].split('\n')
                  if c.strip().startswith('-') and c.strip('- ').strip()]

    args = re.findall(r'^\s+args (.*)$', text, re.M)
    fails = re.findall(r'^\s{4,}FAIL\s+(.*)$', text, re.M)
    repeats = {}
    for a in args:
        repeats[a] = repeats.get(a, 0) + 1

    stop = field(text, 'stop reason') or ''
    verification = field(text, 'verification')
    completed = field(text, 'completed')

    rates = RATE.findall(text)
    decode = [float(d) for d, _ in rates]
    prompt_rate = [float(p) for _, p in rates]

    # Criteria the run itself reported as never settled, counted from the
    # semicolon-separated list it prints rather than assumed to be zero.
    unsettled = 0
    m = UNSETTLED.search(text)
    if m:
        unsettled = len([s for s in m.group(1).split(';') if s.strip()])

    return {
        'log': os.path.basename(path),
        'completed': completed,
        'verification': verification,
        'stop_reason': stop,
        # Presentation, read from what the run reported about itself.
        'empty_answer': 'empty answer' in stop or 'no final answer' in stop,
        'finalized': 'asked directly' in stop,
        'presentation_failed': 'verified, but' in stop and 'no final answer' in stop,
        'clean': completed == 'yes' and verification == 'verified' and not claims,
        'steps': int(field(text, 'steps') or 0),
        'tool_calls': int(field(text, 'tool calls') or 0),
        'duration_s': duration_seconds(field(text, 'duration')),
        'claims': claims,
        'invented_locations': sum(1 for a in args if BOGUS.search(a)),
        'invalid_calls': sum(1 for f in fails if INVALID.search(f)),
        'repeated_calls': sum(v - 1 for v in repeats.values() if v > 1),
        'gate_interventions': len(STALL_NUDGE.findall(text)),
        'finalizer_entered': len(FINALIZER_ENTERED.findall(text)),
        'criteria_unsettled': unsettled,
        'decode_tok_s': st.median(decode) if decode else None,
        'prompt_tok_s': st.median(prompt_rate) if prompt_rate else None,
        'model_calls': len(rates),
    }


def summarise(directory):
    logs = sorted(glob.glob(os.path.join(directory, 'logs', '*.log')),
                  key=lambda p: int(re.search(r'(\d+)', os.path.basename(p)).group(1)))
    rows = [parse(p) for p in logs]
    if not rows:
        return None, []

    n = len(rows)
    verified = sum(1 for r in rows if r['verification'] == 'verified')
    clean = sum(1 for r in rows if r['clean'])
    empty = sum(1 for r in rows if r['empty_answer'])
    finalized = sum(1 for r in rows if r['finalized'])
    pres_failed = sum(1 for r in rows if r['presentation_failed'])
    calls = sum(r['tool_calls'] for r in rows)
    durations = [r['duration_s'] for r in rows if r['duration_s']]
    median_s = st.median(durations) if durations else 0
    decode = [r['decode_tok_s'] for r in rows if r['decode_tok_s']]
    prompt_rate = [r['prompt_tok_s'] for r in rows if r['prompt_tok_s']]
    entered = sum(1 for r in rows if r['finalizer_entered'])

    summary = {
        'runs': n,
        'verified_task_rate': verified / n,
        'clean_completion_rate': clean / n,
        'final_emitted_rate': (n - empty) / n,
        'finalization_recovered': finalized,
        'finalizer_entered': entered,
        'presentation_failed': pres_failed,
        'criteria_unsettled': sum(r['criteria_unsettled'] for r in rows),
        'median_decode_tok_s': st.median(decode) if decode else None,
        'median_prompt_tok_s': st.median(prompt_rate) if prompt_rate else None,
        'runs_with_unsupported_claims': sum(1 for r in rows if r['claims']),
        'invented_locations': sum(r['invented_locations'] for r in rows),
        'invalid_calls': sum(r['invalid_calls'] for r in rows),
        'repeated_calls': sum(r['repeated_calls'] for r in rows),
        'gate_interventions': sum(r['gate_interventions'] for r in rows),
        'total_tool_calls': calls,
        'median_steps': st.median([r['steps'] for r in rows]),
        'median_tool_calls': st.median([r['tool_calls'] for r in rows]),
        'median_duration_s': median_s,
        'verified_tasks_per_hour': (3600 / median_s * verified / n) if median_s else 0,
        'clean_tasks_per_hour': (3600 / median_s * clean / n) if median_s else 0,
    }
    return summary, rows


def main(argv):
    if len(argv) < 2:
        print('usage: benchmark_metrics.py <results-directory> [...]')
        return 1
    for directory in argv[1:]:
        summary, rows = summarise(directory)
        if not summary:
            print('%s: no logs' % directory)
            continue

        label = os.path.basename(directory.rstrip('\\/'))
        print('=== %s ===' % label)
        for r in rows:
            note = ''
            if r['presentation_failed']:
                note = 'VERIFIED, no summary'
            elif r['finalized']:
                note = 'finalized'
            elif r['empty_answer']:
                note = 'empty'
            print('  %-10s %-4s %-13s steps %2d calls %2d %6.0fs  %s' %
                  (r['log'], r['completed'], r['verification'], r['steps'],
                   r['tool_calls'], r['duration_s'] or 0, note))

        print('  ENGINEERING')
        print('    verified-task rate      : %d/%d = %.0f%%' %
              (round(summary['verified_task_rate'] * summary['runs']), summary['runs'],
               100 * summary['verified_task_rate']))
        print('    runs w/ unsupported     : %d' % summary['runs_with_unsupported_claims'])
        print('    invented locations      : %d of %d calls' %
              (summary['invented_locations'], summary['total_tool_calls']))
        print('    invalid tool calls      : %d of %d calls' %
              (summary['invalid_calls'], summary['total_tool_calls']))
        print('  PRESENTATION')
        print('    clean-completion rate   : %d/%d = %.0f%%' %
              (round(summary['clean_completion_rate'] * summary['runs']), summary['runs'],
               100 * summary['clean_completion_rate']))
        print('    final emitted rate      : %.0f%%' % (100 * summary['final_emitted_rate']))
        print('    finalizer entered       : %d' % summary['finalizer_entered'])
        print('    recovered by finalizer  : %d' % summary['finalization_recovered'])
        print('    presentation failures   : %d' % summary['presentation_failed'])
        print('    criteria left unsettled : %d' % summary['criteria_unsettled'])
        print('  COST')
        print('    median steps / calls    : %d / %d' %
              (summary['median_steps'], summary['median_tool_calls']))
        print('    gate interventions      : %d' % summary['gate_interventions'])
        print('    repeated calls          : %d' % summary['repeated_calls'])
        print('    median duration         : %.0fs' % summary['median_duration_s'])
        print('    median decode / prompt  : %s / %s tok/s' %
              ('%.1f' % summary['median_decode_tok_s'] if summary['median_decode_tok_s'] else '-',
               '%.0f' % summary['median_prompt_tok_s'] if summary['median_prompt_tok_s'] else '-'))
        print('    verified tasks/hour     : %.1f' % summary['verified_tasks_per_hour'])
        print('    clean tasks/hour        : %.1f' % summary['clean_tasks_per_hour'])
        print()

        out = os.path.join(directory, 'metrics.json')
        io.open(out, 'w', encoding='utf-8').write(
            json.dumps({'summary': summary, 'runs': rows}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
