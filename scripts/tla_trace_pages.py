#!/usr/bin/env python3
"""
tla_trace_pages.py -- Reproduce the counterexamples of MajorityLeaseHA.tla with the TLC model checker,
confirm that each one still fails, and write them out as text traces and as a web page that steps
through them.

MajorityLeaseHA.tla (in docs/availability/tla/) specifies the proposed design for deciding which instance
of a pair leads. Most of what was learned from checking it is recorded as counterexamples: runs in which
one rule of the design is removed or changed, and TLC finds a sequence of events that breaks a property.
Each of those runs is evidence that the rule is needed. This script reruns every one of them and fails
if any no longer breaks the property it is listed against. A counterexample that stops failing means the
specification has changed in a way that no longer shows what findings.md section 11 says it shows, and
that has to be looked at, not discovered months later.

For each counterexample the script:
  1. writes a TLC configuration from MajorityLeaseHA.cfg, with the constants listed below changed and the
     property to check replaced;
  2. runs TLC with one worker thread, so that the counterexample it reports is the same on every run;
  3. checks that TLC reports the expected property as broken;
  4. converts TLC's description of each state into a table row, and into a plain English caption made
     by comparing the state with the one before it.

It then writes, into the output directory:
  - one text file per counterexample, a table of states, named as in docs/availability/tla/traces/;
  - majority_lease_counterexamples.html, a self-contained page that steps through all of them.

The committed text traces of the safety counterexamples in docs/availability/tla/traces/ are compared
with the ones generated. A difference fails the run, with a message saying which file differs. Run with
--update-traces to replace the committed files after a deliberate change to the specification. The
liveness counterexamples are not compared: TLC reports one of several loops that break a liveness
property, and which one varies from run to run, so those must break their property but need not match.

The page's layout is in tla_trace_page.html beside this script. The captions below know what each
variable of MajorityLeaseHA.tla means, so a counterexample from another specification needs caption
rules of its own before it can be added here.

TLC is in tla2tools.jar, from https://github.com/tlaplus/tlaplus/releases. The build uses release
v1.7.4. The jar is found from --jar, then the TLA2TOOLS_JAR environment variable, then
$THIRDPARTY_DIR/tla2tools-1.7.4/tla2tools.jar. It needs Java 11 or later.

To add a counterexample, add an entry to COUNTEREXAMPLES: the constants to change, the specification
and the property to check, the property TLC must report broken, and the words the page shows. Then run
this script with --update-traces and commit the new trace file.

Usage:
  scripts/tla_trace_pages.py --output-dir build/tla_pages
  scripts/tla_trace_pages.py --output-dir build/tla_pages --update-traces
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_TLA_DIR = _PROJECT_ROOT / 'docs' / 'availability' / 'tla'
_TRACES_DIR = _TLA_DIR / 'traces'
_SPEC = 'MajorityLeaseHA'
_TEMPLATE = Path(__file__).resolve().parent / 'tla_trace_page.html'
_PAGE_NAME = 'majority_lease_counterexamples.html'
_TLA_TOOLS_VERSION = '1.7.4'
_TLC_TIMEOUT_SECONDS = 300

# The constants shared by the runs below, before each run's own changes. Each run allows one generation
# of epochs and only the failures its counterexample needs. That keeps the search small, so that if a
# change ever stops a counterexample failing, TLC exhausts the states within about two minutes and says it found
# no violation, rather than searching for hours.
_NO_FAILURES = {'MaxEpoch': '0', 'MaxCrashes': '0', 'MaxThirdRestarts': '0', 'MaxLinkFailures': '0'}
_ARBITER_DOWN_FOR_GOOD = dict(_NO_FAILURES, Prompt='TRUE', ThirdUpAtStart='FALSE', ThirdStaysDown='TRUE')


@dataclass
class Counterexample:
    """One run of TLC that must break one property, and the words the page shows for it."""
    name: str                      # the trace file is <name>.txt
    constants: Dict[str, str]      # constants changed from MajorityLeaseHA.cfg
    specification: str             # Spec for safety, LiveSpec for liveness
    check: str                     # the INVARIANTS or PROPERTY line of the configuration
    broken: str                    # the property TLC must report broken
    title: str
    change: str
    summary: str


COUNTEREXAMPLES: List[Counterexample] = [
    Counterexample(
        'lease-1-holder-counts-from-arrival', dict(_NO_FAILURES, HolderCountsFromSend='FALSE'),
        'Spec', 'INVARIANTS AtMostOneActing', 'AtMostOneActing',
        'A lease counted from the wrong moment',
        'Rule 2 removed: the leader counts its lease from when the grant arrived, instead of from when it sent the request.',
        'Instance 2 granted instance 1 a lease, but the grant took a tick to arrive. Instance 1 counts from the arrival, so '
        'it believes its lease lasts a tick longer than instance 2 believes its promise does. In that gap instance 2 asks to '
        'lead, the arbiter grants it, and both act as leader.'),
    Counterexample(
        'lease-2-candidate-ignores-own-grant', dict(_NO_FAILURES, WaitOutOwnGrant='FALSE'),
        'Spec', 'INVARIANTS AtMostOneActing', 'AtMostOneActing',
        'Asking to lead while a grant is still live',
        'Rule 5 removed: an instance may ask to lead while its grant to its peer has not run out.',
        'Instance 2 grants instance 1 a lease and at once asks to lead itself. The arbiter grants instance 2. Each now has '
        'two of the three votes, counting its own, and both act as leader.'),
    Counterexample(
        'lease-3-restart-does-not-wait', dict(_NO_FAILURES, MaxCrashes='1', RestartWaits='FALSE'),
        'Spec', 'INVARIANTS AtMostOneActing', 'AtMostOneActing',
        'A restart that forgets a promise',
        'Rule 6 removed: a restarted voter grants votes straight away.',
        'Instance 2 grants instance 1 a lease, crashes, and restarts having forgotten the promise. It asks to lead, the '
        'arbiter grants it, and instance 1 is still acting.'),
    Counterexample(
        'lease-4-degraded-promotion', dict(_NO_FAILURES, DegradedPromotion='TRUE'),
        'Spec', 'INVARIANTS AtMostOneActing', 'AtMostOneActing',
        'Degraded self-promotion',
        'Promotion without a majority added back.',
        'Each instance asks to lead and, with no second vote, promotes itself on its own vote alone. This is the '
        'behaviour the proposal removes.'),
    Counterexample(
        'lease-5-epoch-regresses', dict(_NO_FAILURES, MaxThirdRestarts='1'),
        'Spec', 'INVARIANTS NoRegression', 'NoRegression',
        'An epoch goes backwards',
        'Nothing: this is the design as proposed. The property checked is that no leader begins below an epoch already led in.',
        'Instance 2 leads at epoch 2 with the arbiter\'s vote. The arbiter restarts and forgets epoch 2. Instance 2\'s lease '
        'runs out, and then instance 1 is elected at epoch 1. Two leaders never act at once, but receivers that saw epoch 2 '
        'would ignore epoch 1 until rule 8 makes instance 1 ask again above it.'),
    Counterexample(
        'lease-6-no-yield-livelock', dict(_ARBITER_DOWN_FOR_GOOD, CandidateYields='FALSE'),
        'LiveSpec', 'PROPERTY EventuallyLeaderForGood', 'EventuallyLeaderForGood',
        'Two candidates refusing each other for ever',
        'Rule 9 removed, with the arbiter down: a candidate does not give way to a request at a higher epoch.',
        'Both instances ask to lead at the same moment. Each has voted for itself, so each refuses the other, both give up, '
        'and the same thing happens again, for ever. Nobody ever leads.'),
    Counterexample(
        'lease-7-no-peer-vote', dict(_ARBITER_DOWN_FOR_GOOD, PeerVotes='FALSE'),
        'LiveSpec', 'PROPERTY EventuallyLeaderForGood', 'EventuallyLeaderForGood',
        'No vote from the peer',
        'The peer\'s vote removed, with the arbiter down: only the arbiter can supply a second vote.',
        'With the arbiter down and the instances not voting for each other, no instance can ever gather two of the three '
        'votes, so nobody leads. This is why the peer\'s vote is what keeps trading going when the arbiters are lost.'),
]

# Voter 3 is the arbiter when the design is applied to a component pair, and the witness when it is
# applied to the arbiters. The page describes the component pair.
_NAMES = {1: 'Instance 1', 2: 'Instance 2', 3: 'The arbiter'}


class TraceError(Exception):
    """A counterexample could not be reproduced as recorded."""


# --------------------------------------------------------------------------------------------------
# Running TLC
# --------------------------------------------------------------------------------------------------

def find_jar(explicit: Optional[str]) -> Path:
    """Locate tla2tools.jar from the command line, the environment, or the third-party directory."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get('TLA2TOOLS_JAR'):
        candidates.append(Path(os.environ['TLA2TOOLS_JAR']))
    if os.environ.get('THIRDPARTY_DIR'):
        candidates.append(Path(os.environ['THIRDPARTY_DIR']) / f'tla2tools-{_TLA_TOOLS_VERSION}' / 'tla2tools.jar')
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    searched = ', '.join(str(c) for c in candidates) or 'nowhere: no --jar, TLA2TOOLS_JAR or THIRDPARTY_DIR given'
    raise TraceError(f'tla2tools.jar not found. Looked in: {searched}')


def make_config(base: str, example: Counterexample) -> str:
    """The base configuration with the example's constants changed and its property to check in place."""
    lines = base.split('\n')
    for name, value in example.constants.items():
        pattern = re.compile(r'^(\s+)' + re.escape(name) + r'\s*=.*$')
        matches = [i for i, line in enumerate(lines) if pattern.match(line)]
        if len(matches) != 1:
            # A misspelt constant would otherwise leave the default in place, and the run would check
            # something other than what the entry says.
            raise TraceError(f'{example.name}: constant {name} appears {len(matches)} times in {_SPEC}.cfg, not once')
        indent = pattern.match(lines[matches[0]]).group(1)
        lines[matches[0]] = f'{indent}{name} = {value}'
    text = '\n'.join(lines)
    head = text[:text.index('\nSPECIFICATION')]
    return f'{head}\nSPECIFICATION {example.specification}\nCONSTRAINT EpochBound\n{example.check}\n'


def run_tlc(java: str, jar: Path, example: Counterexample) -> str:
    """Run TLC on the example in a scratch directory and return everything it printed."""
    base = (_TLA_DIR / f'{_SPEC}.cfg').read_text()
    with tempfile.TemporaryDirectory(prefix='tla_trace_') as work:
        work_dir = Path(work)
        shutil.copy(_TLA_DIR / f'{_SPEC}.tla', work_dir)
        (work_dir / f'{_SPEC}.cfg').write_text(make_config(base, example))
        # One worker, so that the reported counterexample is the same on every run. -metadir keeps
        # TLC's state files inside the scratch directory.
        command = [java, '-XX:+UseParallelGC', '-cp', str(jar), 'tlc2.TLC', '-deadlock', '-workers', '1',
                   '-metadir', str(work_dir / 'states'), '-config', f'{_SPEC}.cfg', f'{_SPEC}.tla']
        try:
            result = subprocess.run(command, cwd=work_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    universal_newlines=True, timeout=_TLC_TIMEOUT_SECONDS, check=False)
        except subprocess.TimeoutExpired as expired:
            raise TraceError(f'{example.name}: TLC searched for {_TLC_TIMEOUT_SECONDS} seconds without finding a violation '
                             f'of {example.broken}. Each counterexample normally fails within seconds, so this one has '
                             f'probably stopped failing.') from expired
    return result.stdout


def broken_property(example: Counterexample, output: str) -> Optional[str]:
    """The property TLC reported broken, or None if it reported none.

    TLC names a broken invariant. Some releases do not name a broken temporal property, and say only
    "Temporal properties were violated"; each liveness run here checks exactly one property, so that
    one is the property broken.
    """
    named = re.search(r'Error: (?:Invariant|Temporal property) (\w+) (?:is|was) violated', output)
    if named:
        return named.group(1)
    if 'Error: Temporal properties were violated' in output and example.check.startswith('PROPERTY '):
        return example.check.split()[1]
    return None


def violation_text(example: Counterexample, output: str) -> str:
    """How TLC's report reads, with the broken property always named."""
    kind = 'Temporal property' if example.specification == 'LiveSpec' else 'Invariant'
    verb = 'was' if example.specification == 'LiveSpec' else 'is'
    return f'{kind} {broken_property(example, output)} {verb} violated'


def check_broken(example: Counterexample, output: str) -> None:
    """Fail unless TLC reported the expected property broken."""
    broken = broken_property(example, output)
    if broken == example.broken:
        return
    if 'No error has been found' in output:
        raise TraceError(f'{example.name}: TLC found no violation of {example.broken}. This counterexample no longer '
                         f'fails, so the rule it is listed against is no longer shown to be needed.')
    if broken:
        raise TraceError(f'{example.name}: TLC reported {broken} broken, not {example.broken}')
    tail = '\n'.join(output.strip().split('\n')[-15:])
    raise TraceError(f'{example.name}: TLC did not complete. The end of its output was:\n{tail}')


# --------------------------------------------------------------------------------------------------
# Reading TLC's description of a state
# --------------------------------------------------------------------------------------------------

def parse_value(text: str, pos: int = 0):
    """Parse one TLA+ value as TLC prints it: a sequence, record, set, string, integer or Boolean."""
    def skip(i):
        while i < len(text) and text[i] in ' \n\t':
            i += 1
        return i

    def items(i, close, parse_item):
        out = []
        i = skip(i)
        if text.startswith(close, i):
            return out, i + len(close)
        while True:
            item, i = parse_item(i)
            out.append(item)
            i = skip(i)
            if text.startswith(close, i):
                return out, i + len(close)
            if text[i] != ',':
                raise TraceError(f'unexpected text in a TLC value: {text[i:i + 30]!r}')
            i += 1

    def field(i):
        match = re.match(r'\s*(\w+) \|-> ', text[i:])
        value, end = parse_value(text, i + match.end())
        return (match.group(1), value), end

    pos = skip(pos)
    if text.startswith('<<', pos):
        return items(pos + 2, '>>', lambda i: parse_value(text, i))
    if text[pos] == '[':
        fields, end = items(pos + 1, ']', field)
        return dict(fields), end
    if text[pos] == '{':
        return items(pos + 1, '}', lambda i: parse_value(text, i))
    if text[pos] == '"':
        end = text.index('"', pos + 1)
        return text[pos + 1:end], end + 1
    match = re.match(r'TRUE|FALSE|-?\d+', text[pos:])
    if not match:
        raise TraceError(f'unexpected text in a TLC value: {text[pos:pos + 30]!r}')
    token = match.group(0)
    value = (token == 'TRUE') if token in ('TRUE', 'FALSE') else int(token)
    return value, pos + match.end()


def read_states(output: str) -> Tuple[List[dict], Optional[int]]:
    """Every state of the counterexample, and for a liveness counterexample the state it returns to."""
    states = []
    loop = None
    for block in re.split(r'\n(?=State \d+:|Back to state)', output):
        back = re.match(r'Back to state (\d+)', block)
        if back:
            loop = int(back.group(1))
            continue
        header = re.match(r'State (\d+): <(\w+)(?:\(([^)]*)\))?', block)
        if not header:
            continue
        state = {'action': header.group(2), 'arg': header.group(3)}
        for variable in re.finditer(r'^/\\ (\w+) = (.*?)(?=\n/\\ |\Z)', block, re.S | re.M):
            state[variable.group(1)] = parse_value(variable.group(2).strip())[0]
        states.append(state)
    if not states:
        raise TraceError('TLC reported a violation but printed no states')
    for previous, state in zip(states, states[1:]):
        if state['arg'] is None:
            state['arg'] = _whom(previous, state)
    return states, loop


def _whom(previous: dict, state: dict) -> Optional[str]:
    """Which instance, voter or link a step acted on, worked out from what changed.

    Some releases of TLC name the action of each step but not its argument, so "Crash" arrives without
    saying who crashed. Every parameterised action changes something that identifies its argument.
    """
    action = state['action']
    if action in ('StartCandidacy', 'AbandonCandidacy', 'PromoteAlone', 'LeaseRunsOut'):
        changed = [i + 1 for i in range(2) if previous['role'][i] != state['role'][i]]
        return str(changed[0]) if changed else None
    if action in ('Crash', 'Restart'):
        changed = [i + 1 for i in range(3) if previous['up'][i] != state['up'][i]]
        return str(changed[0]) if changed else None
    if action in ('LinkFails', 'LinkRecovers'):
        changed = sorted(set(previous['link']) ^ set(state['link']))
        return f'"{changed[0]}"' if changed else None
    return None


# --------------------------------------------------------------------------------------------------
# Describing a step in words
# --------------------------------------------------------------------------------------------------

def acting(state: dict, instance: int) -> bool:
    """Whether the instance acts as leader: it leads and holds an unexpired grant, or leads alone."""
    i = instance - 1
    return state['role'][i] == 'leader' and (state['alone'][i] or any(h > 0 for h in state['held'][i]))


def _message_key(message: dict) -> tuple:
    return (message['type'], message['from'], message['to'], message['epoch'])


def _describe_arrival(previous: dict, state: dict, no_peer_votes: bool) -> str:
    """Caption for a step in which a message arrived, found by which message left the set in flight."""
    before = {_message_key(m) for m in previous['msgs']}
    after = {_message_key(m) for m in state['msgs']}
    gone = [m for m in previous['msgs'] if _message_key(m) not in after]
    if not gone:
        return 'A step with no visible effect.'
    message = gone[0]
    sender, receiver, epoch = message['from'], message['to'], message['epoch']
    sender_name, receiver_name = _NAMES[sender], _NAMES[receiver]
    if not previous['up'][receiver - 1]:
        return f'{sender_name}\'s message to {receiver_name.lower()} is lost, because {receiver_name.lower()} is down.'
    if message['type'] == 'req':
        granted = any(k[0] == 'ack' and k[1] == receiver and k[2] == sender for k in after - before)
        if granted:
            return (f'{receiver_name} receives {sender_name.lower()}\'s request for epoch {epoch} and grants it, '
                    f'promising its vote to {sender_name.lower()} for the lease period.')
        promise = previous['promise'][receiver - 1]
        reason = ''
        if no_peer_votes and receiver in (1, 2):
            reason = ' In this run the instances do not vote for each other.'
        elif promise['left'] > 0 and promise['to'] != sender:
            reason = (' It is asking to lead itself.' if promise['to'] == receiver
                      else f' It has promised its vote to {_NAMES[promise["to"]].lower()}.')
        elif previous['quiet'][receiver - 1] > 0:
            reason = ' It restarted recently and is still waiting.'
        elif epoch < previous['epoch'][receiver - 1]:
            reason = f' It has already granted epoch {previous["epoch"][receiver - 1]}, which is higher.'
        return f'{receiver_name} receives {sender_name.lower()}\'s request for epoch {epoch} and refuses it.{reason}'
    if message['type'] == 'ack':
        if previous['role'][receiver - 1] == 'candidate' and state['role'][receiver - 1] == 'leader':
            return (f'{receiver_name} receives the grant from {sender_name.lower()}. With its own vote that is two of '
                    f'three, so it becomes leader at epoch {epoch}.')
        return f'{receiver_name} receives a grant from {sender_name.lower()} and extends its lease.'
    return f'{receiver_name} receives the refusal from {sender_name.lower()}.'


def caption(previous: Optional[dict], state: dict, no_peer_votes: bool) -> str:
    """One or two plain English sentences saying what happened at this step."""
    action, arg = state['action'], state['arg']
    if action == 'Initial':
        if state['up'][2]:
            return 'Start. Both instances are running with no role, and nobody has promised a vote to anyone.'
        return 'Start. Both instances are running with no role. The arbiter is down.'
    if action == 'Tick':
        text = 'One tick of the clock passes. Every promise, lease and restart wait counts down by one.'
        for instance in (1, 2):
            if acting(previous, instance) and not acting(state, instance):
                text += f' {_NAMES[instance]}\'s lease has now run out, so it no longer acts as leader.'
        return text
    if action == 'StartCandidacy':
        who = int(arg)
        return (f'{_NAMES[who]} asks to lead, at epoch {state["candEpoch"][who - 1]}. It votes for itself and asks the '
                f'other two voters for their votes.')
    if action == 'AbandonCandidacy':
        return f'{_NAMES[int(arg)]} has not gathered a second vote, so it gives up asking and releases its own vote.'
    if action == 'PromoteAlone':
        return f'{_NAMES[int(arg)]} makes itself leader on its own vote alone (degraded self-promotion).'
    if action == 'LeaseRunsOut':
        return f'{_NAMES[int(arg)]} has no unexpired lease left, so it stops leading.'
    if action == 'Crash':
        who = int(arg)
        if who == 3:
            return (f'{_NAMES[who]} crashes. It loses everything it held in memory, including the highest epoch it '
                    f'had granted.')
        return f'{_NAMES[who]} crashes. Its epoch is on disk and survives; its promises do not.'
    if action == 'Restart':
        who = int(arg)
        wait = state['quiet'][who - 1]
        if wait:
            return f'{_NAMES[who]} restarts. It will grant nothing for {wait} ticks, because it has forgotten what it promised.'
        return f'{_NAMES[who]} restarts. It grants votes straight away, although it has forgotten what it promised.'
    if action in ('LinkFails', 'LinkRecovers'):
        return f'The link {arg} {"fails" if action == "LinkFails" else "recovers"}.'
    if action == 'SendRenewal':
        return 'The leader asks for its lease to be renewed.'
    # TLC names every message arrival after the whole next-state relation, so it is worked out from the
    # messages in flight.
    return _describe_arrival(previous, state, no_peer_votes)


# --------------------------------------------------------------------------------------------------
# Writing the outputs
# --------------------------------------------------------------------------------------------------

def _flags(values: List[bool]) -> str:
    return '<<' + ','.join('T' if v else 'F' for v in values) + '>>'


def _numbers(values: list) -> str:
    return '<<' + ','.join(str(v) for v in values) + '>>'


def text_trace(example: Counterexample, output: str, states: List[dict], loop: Optional[int]) -> str:
    """The counterexample as a table of states, one line per step."""
    lines = [
        f'Error: {violation_text(example, output)}.',
        'Columns. up, epoch, promise and quiet list voters 1, 2 and 3 in that order; role lists instances 1 and 2.',
        'promise "toX:N" means the voter has promised its vote to instance X for N more ticks, and "-" means no promise.',
        'held lists, for instance 1 and then instance 2, its own count of the ticks left on the grant from each voter 1, 2, 3.',
        'quiet is the ticks left in which a newly restarted voter grants nothing. msgs lists messages in flight as',
        '"type from>to eEPOCH". An instance acts as leader only while its role is lead and one of its own held counts is',
        'above zero, so "lead" with every held count at zero is a leader whose lease has run out and that has not yet tidied up.',
        f'Reproduced by scripts/tla_trace_pages.py, entry {example.name}.',
    ]
    for number, state in enumerate(states, start=1):
        action = state['action'] + (f'({state["arg"]})' if state['arg'] else '')
        if action == 'Next':
            action = 'a message arrives'
        promise = ' '.join(f'to{p["to"]}:{p["left"]}' if p['left'] else '-' for p in state['promise'])
        held = '<<' + ','.join(_numbers(h) for h in state['held']) + '>>'
        messages = '{' + ', '.join(sorted(f'{m["type"]} {m["from"]}>{m["to"]} e{m["epoch"]}' for m in state['msgs'])) + '}'
        roles = ','.join(r[:4] for r in state['role'])
        links = '{' + ','.join(sorted(state['link'])) + '}'
        lines.append(f'{number:3} {action:22} up={_flags(state["up"]):14} role={roles:10} epoch={_numbers(state["epoch"]):8} '
                     f'promise={promise:12} held={held:24} quiet={_numbers(state["quiet"]):6} links={links:18} msgs={messages}')
    if loop is not None:
        lines.append(f'    Back to state {loop} (the behaviour repeats from there for ever)')
    return '\n'.join(line.rstrip() for line in lines) + '\n'


def page_data(example: Counterexample, output: str, states: List[dict], loop: Optional[int]) -> dict:
    """What the page needs for one counterexample."""
    broken = violation_text(example, output)
    no_peer_votes = example.constants.get('PeerVotes') == 'FALSE'
    steps = []
    tick = 0
    for index, state in enumerate(states):
        if state['action'] == 'Tick':
            tick += 1
        steps.append({
            'caption': caption(states[index - 1] if index else None, state, no_peer_votes),
            'tick': tick,
            'up': state['up'], 'role': state['role'], 'epoch': state['epoch'], 'candEpoch': state['candEpoch'],
            'promise': state['promise'], 'held': state['held'], 'alone': state['alone'], 'quiet': state['quiet'],
            'links': state['link'],
            'msgs': [[m['type'], m['from'], m['to'], m['epoch'], m['rem']] for m in state['msgs']],
        })
    return {'id': example.name, 'title': example.title, 'change': example.change, 'summary': example.summary,
            'violation': broken, 'liveness': example.specification == 'LiveSpec', 'loop': loop, 'steps': steps}


def lease_ticks() -> str:
    """The lease period in MajorityLeaseHA.cfg, which the page states."""
    match = re.search(r'^\s+Lease\s*=\s*(\d+)', (_TLA_DIR / f'{_SPEC}.cfg').read_text(), re.M)
    if not match:
        raise TraceError(f'no Lease constant in {_SPEC}.cfg')
    return match.group(1)


def main() -> int:
    """Reproduce every counterexample, write the outputs, and return the process exit status."""
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n', maxsplit=1)[0].strip(),
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output-dir', required=True, help='Where to write the page and the generated text traces')
    parser.add_argument('--jar', help='Path to tla2tools.jar')
    parser.add_argument('--java', default=None, help='Java executable (default: $JAVA_HOME/bin/java, else java)')
    parser.add_argument('--update-traces', action='store_true',
                        help='Replace the committed text traces with the generated ones, instead of comparing them')
    args = parser.parse_args()

    java = args.java or (str(Path(os.environ['JAVA_HOME']) / 'bin' / 'java') if os.environ.get('JAVA_HOME') else 'java')
    output_dir = Path(args.output_dir).resolve()
    try:
        jar = find_jar(args.jar)
        output_dir.mkdir(parents=True, exist_ok=True)
        pages = []
        differing = []
        for example in COUNTEREXAMPLES:
            output = run_tlc(java, jar, example)
            check_broken(example, output)
            states, loop = read_states(output)
            text = text_trace(example, output, states, loop)
            (output_dir / f'{example.name}.txt').write_text(text)
            committed = _TRACES_DIR / f'{example.name}.txt'
            if args.update_traces:
                committed.write_text(text)
            elif example.specification == 'Spec' and (not committed.is_file() or committed.read_text() != text):
                # Only safety counterexamples are compared. For a liveness property TLC reports one of
                # several loops that break it, and which one varies from run to run even with one worker,
                # so a liveness trace is required to break its property but not to match the committed file.
                differing.append(committed)
            pages.append(page_data(example, output, states, loop))
            print(f'  {example.name}: {example.broken} broken, as expected ({len(states)} states)')
        page = _TEMPLATE.read_text().replace('__TRACES_JSON__', json.dumps(pages, separators=(',', ':')))
        page = page.replace('__LEASE_TICKS__', lease_ticks()).replace('__TLA_TOOLS_VERSION__', _TLA_TOOLS_VERSION)
        (output_dir / _PAGE_NAME).write_text(page)
        print(f'Wrote {output_dir / _PAGE_NAME}')
        if differing:
            names = '\n  '.join(str(p.relative_to(_PROJECT_ROOT)) for p in differing)
            raise TraceError('these committed traces differ from what TLC now produces:\n  ' + names +
                             f'\nThe generated versions are in {output_dir}. If the specification was changed on purpose, '
                             'rerun with --update-traces and commit the result.')
    except (TraceError, OSError, subprocess.SubprocessError) as error:
        print(f'tla_trace_pages.py: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
