#!/usr/bin/env python3
"""
wM-Buster ADV driver generator.

Reads the upstream wmbusmeters driver sources (drivers/src/*.xmq and
drivers/library.xmq) and generates:

  lib/wmbus/src/drivers_generated.cpp    driver rule tables (+ compiled ixml grammars)
  lib/wmbus/src/manufacturers_generated.cpp  manufacturer names (src/manufacturers.h)
  test/vectors/driver_tests.h            upstream test telegrams + expected json

Usage:
    tools/generate_drivers.py [path/to/wmbusmeters]
"""

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Tables mirroring lib/wmbus/include/wmbus/types.h
# ---------------------------------------------------------------------------
QUANTITIES = ['Time', 'Length', 'Mass', 'Amperage', 'Temperature', 'AmountOfSubstance',
              'LuminousIntensity', 'Energy', 'Reactive_Energy', 'Apparent_Energy', 'Power',
              'Reactive_Power', 'Apparent_Power', 'Volume', 'Flow', 'Voltage', 'Frequency',
              'Pressure', 'PointInTime', 'RelativeHumidity', 'HCA', 'Text', 'Angle', 'Dimensionless']

# (cname, lcname, quantity)
UNITS = [
    ('Second', 's', 'Time'), ('M', 'm', 'Length'), ('KG', 'kg', 'Mass'), ('Ampere', 'a', 'Amperage'),
    ('K', 'k', 'Temperature'), ('MOL', 'mol', 'AmountOfSubstance'), ('CD', 'cd', 'LuminousIntensity'),
    ('WH', 'wh', 'Energy'), ('KWH', 'kwh', 'Energy'), ('MJ', 'mj', 'Energy'), ('GJ', 'gj', 'Energy'),
    ('GCAL', 'gcal', 'Energy'), ('KVARH', 'kvarh', 'Reactive_Energy'), ('KVAH', 'kvah', 'Apparent_Energy'),
    ('M3C', 'm3c', 'Energy'), ('W', 'w', 'Power'), ('KW', 'kw', 'Power'), ('JH', 'jh', 'Power'),
    ('MJH', 'mjh', 'Power'), ('KVAR', 'kvar', 'Reactive_Power'), ('KVA', 'kva', 'Apparent_Power'),
    ('M3CH', 'm3ch', 'Power'), ('DBM', 'dbm', 'Power'), ('M3', 'm3', 'Volume'), ('L', 'l', 'Volume'),
    ('M3H', 'm3h', 'Flow'), ('LH', 'lh', 'Flow'), ('C', 'c', 'Temperature'), ('F', 'f', 'Temperature'),
    ('Volt', 'v', 'Voltage'), ('HZ', 'hz', 'Frequency'), ('PA', 'pa', 'Pressure'), ('BAR', 'bar', 'Pressure'),
    ('Minute', 'min', 'Time'), ('Hour', 'h', 'Time'), ('Day', 'd', 'Time'), ('Month', 'month', 'Time'),
    ('Year', 'y', 'Time'), ('UnixTimestamp', 'ut', 'PointInTime'), ('DateTimeUTC', 'utc', 'PointInTime'),
    ('DateTimeLT', 'datetime', 'PointInTime'), ('DateLT', 'date', 'PointInTime'), ('TimeLT', 'time', 'PointInTime'),
    ('RH', 'rh', 'RelativeHumidity'), ('HCA', 'hca', 'HCA'), ('TXT', 'txt', 'Text'), ('DEGREE', 'deg', 'Angle'),
    ('RADIAN', 'rad', 'Angle'), ('COUNTER', 'counter', 'Dimensionless'), ('FACTOR', 'factor', 'Dimensionless'),
    ('NUMBER', 'nr', 'Dimensionless'), ('PERCENTAGE', 'pct', 'Dimensionless'), ('PPM', 'ppm', 'Dimensionless'),
]
UNIT_BY_NAME = {}
for c, l, q in UNITS:
    UNIT_BY_NAME[c] = c
    UNIT_BY_NAME[l] = c
    UNIT_BY_NAME[c.lower()] = c

QUANTITY_DEFAULT_UNIT = {
    'Time': 'Hour', 'Length': 'M', 'Mass': 'KG', 'Amperage': 'Ampere', 'Temperature': 'C',
    'AmountOfSubstance': 'MOL', 'LuminousIntensity': 'CD', 'Energy': 'KWH', 'Reactive_Energy': 'KVARH',
    'Apparent_Energy': 'KVAH', 'Power': 'KW', 'Reactive_Power': 'KVAR', 'Apparent_Power': 'KVA',
    'Volume': 'M3', 'Flow': 'M3H', 'Voltage': 'Volt', 'Frequency': 'HZ', 'Pressure': 'BAR',
    'PointInTime': 'DateTimeLT', 'RelativeHumidity': 'RH', 'HCA': 'HCA', 'Text': 'TXT', 'Angle': 'DEGREE',
    'Dimensionless': 'COUNTER',
}

VIF_RANGES = [
    'Volume', 'OnTime', 'OperatingTime', 'VolumeFlow', 'FlowTemperature', 'ReturnTemperature',
    'TemperatureDifference', 'ExternalTemperature', 'Pressure', 'HeatCostAllocation', 'Date', 'DateTime',
    'EnergyMJ', 'EnergyWh', 'PowerW', 'PowerJh', 'ActualityDuration', 'FabricationNo',
    'EnhancedIdentification', 'EnergyMWh', 'EnergyGJ', 'RelativeHumidity', 'AccessNumber', 'Medium',
    'Manufacturer', 'ParameterSet', 'ModelVersion', 'HardwareVersion', 'FirmwareVersion', 'SoftwareVersion',
    'Location', 'Customer', 'ErrorFlags', 'DigitalOutput', 'DigitalInput', 'DurationSinceReadout',
    'DurationOfTariff', 'Dimensionless', 'Voltage', 'Amperage', 'ResetCounter', 'CumulationCounter',
    'SpecialSupplierInformation', 'RemainingBattery', 'AnyVolumeVIF', 'AnyEnergyVIF', 'AnyPowerVIF',
]

COMBINABLES = [
    'Reserved', 'Average', 'InverseCompactProfile', 'RelativeDeviation', 'RecordErrorCodeMeterToController',
    'StandardConformantDataContent', 'CompactProfileWithRegister', 'CompactProfile', 'PerSecond', 'PerMinute',
    'PerHour', 'PerDay', 'PerWeek', 'PerMonth', 'PerYear', 'PerRevolutionMeasurement',
    'IncrPerInputPulseChannel0', 'IncrPerInputPulseChannel1', 'IncrPerOutputPulseChannel0',
    'IncrPerOutputPulseChannel1', 'PerLitre', 'PerM3', 'PerKg', 'PerKelvin', 'PerKWh', 'PerGJ', 'PerKW',
    'PerKelvinLitreW', 'PerVolt', 'PerAmpere', 'MultipliedByS', 'MultipliedBySDivV', 'MultipliedBySDivA',
    'StartDateTimeOfAB', 'UncorrectedMeterUnit', 'ForwardFlow', 'BackwardFlow', 'ReservedNonMetric',
    'ValueAtBaseCondC', 'ObisDeclaration', 'LowerLimit', 'ExceedsLowerLimit', 'DateTimeExceedsLowerFirstBegin',
    'DateTimeExceedsLowerFirstEnd', 'DateTimeExceedsLowerLastBegin', 'DateTimeExceedsLowerLastEnd', 'UpperLimit',
    'ExceedsUpperLimit', 'DateTimeExceedsUpperFirstBegin', 'DateTimeExceedsUpperFirstEnd',
    'DateTimeExceedsUpperLastBegin', 'DateTimeExceedsUpperLastEnd', 'DurationExceedsLowerFirst',
    'DurationExceedsLowerLast', 'DurationExceedsUpperFirst', 'DurationExceedsUpperLast', 'DurationOfDFirst',
    'DurationOfDLast', 'ValueDuringLowerLimitExceeded', 'LeakageValues', 'OverflowValues',
    'ValueDuringUpperLimitExceeded', 'DateTimeOfDEFirstBegin', 'DateTimeOfDEFirstEnd', 'DateTimeOfDELastBegin',
    'DateTimeOfDELastEnd', 'MultiplicativeCorrectionFactorForValue', 'AdditiveCorrectionConstant',
    'CombinableVIFExtension', 'MultiplicativeCorrectionFactorForValue103', 'FutureValue', 'MfctSpecific',
    'AtPhase1', 'AtPhase2', 'AtPhase3', 'AtNeutral', 'BetweenPhaseL1AndL2', 'BetweenPhaseL2AndL3',
    'BetweenPhaseL3AndL1', 'AtQuadrantQ1', 'AtQuadrantQ2', 'AtQuadrantQ3', 'AtQuadrantQ4',
    'DeltaBetweenImportAndExport', 'AccumulationOfAbsoluteValue', 'DataPresentedWithTypeC',
    'DataPresentedWithTypeD', 'Mfct00', 'Mfct01', 'Mfct02', 'Mfct03', 'Mfct04', 'Mfct05', 'Mfct06', 'Mfct07',
    'Mfct08', 'Mfct21', 'Mfct72', 'Synthetic',
]
COMBINABLE_INDEX = {'Any': 0}
for i, n in enumerate(COMBINABLES):
    COMBINABLE_INDEX[n] = i + 1

METER_TYPES = ['AutoMeter', 'UnknownMeter', 'DoorWindowDetector', 'ElectricityMeter', 'GasMeter',
               'HeatCoolingMeter', 'HeatCostAllocationMeter', 'HeatMeter', 'PressureSensor', 'PulseCounter',
               'Repeater', 'SmokeDetector', 'TempHygroMeter', 'WaterMeter']

ATTRS = {'HIDE': 0x1, 'STATUS': 0x2, 'INCLUDE_TPL_STATUS': 0x4, 'DEPRECATED': 0x8,
         'INJECT_INTO_STATUS': 0x10, 'REQUIRED': 0x20, 'OPTIONAL': 0x40}


def mfct_code(s: str) -> int:
    if len(s) == 3 and s.isalpha() and s.isupper():
        return ((ord(s[0]) - 64) << 10) | ((ord(s[1]) - 64) << 5) | (ord(s[2]) - 64)
    return int(s, 16)


def crc16_en13757(data: bytes) -> int:
    crc = 0
    for b in data:
        for _ in range(8):
            mix = ((crc >> 8) ^ b) & 0x80
            crc = (crc << 1) & 0xFFFF
            if mix:
                crc ^= 0x3D65
            b = (b << 1) & 0xFF
    return (~crc) & 0xFFFF


# ---------------------------------------------------------------------------
# XMQ parser
# ---------------------------------------------------------------------------
class Node:
    __slots__ = ('name', 'attrs', 'value', 'children')

    def __init__(self, name):
        self.name = name
        self.attrs: Dict[str, str] = {}
        self.value: Optional[str] = None
        self.children: List['Node'] = []

    def get(self, key, default=None):
        for c in self.children:
            if c.name == key and c.value is not None:
                return c.value
        return default

    def all(self, key) -> List['Node']:
        return [c for c in self.children if c.name == key]

    def first(self, key) -> Optional['Node']:
        for c in self.children:
            if c.name == key:
                return c
        return None


def xmq_tokens(s: str):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
            continue
        if s.startswith('//', i):
            j = s.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if s.startswith('/*', i):
            j = s.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in '{}=()':
            yield ('op', c)
            i += 1
            continue
        if c in "'\"":
            # Count the number of consecutive quotes (XMQ multi-quotes).
            q = c
            k = i
            while k < n and s[k] == q:
                k += 1
            cnt = k - i
            if cnt == 2:
                yield ('str', '')
                i = k
                continue
            delim = q * cnt
            j = s.find(delim, k)
            if j < 0:
                raise ValueError('unterminated quote')
            text = s[k:j]
            if cnt >= 3:
                text = dedent_multiline(text)
            yield ('str', text)
            i = j + cnt
            continue
        j = i
        while j < n and not s[j].isspace() and s[j] not in '{}=()\'"':
            j += 1
        yield ('str', s[i:j])
        i = j


def dedent_multiline(text: str) -> str:
    lines = text.split('\n')
    if lines and lines[0].strip() == '':
        lines = lines[1:]
    if lines and lines[-1].strip() == '':
        lines = lines[:-1]
    indent = min((len(l) - len(l.lstrip()) for l in lines if l.strip()), default=0)
    return '\n'.join(l[indent:] for l in lines)


def xmq_unescape(s: str) -> str:
    s = s.replace('&#10;', '\n').replace('&#39;', "'").replace('&quot;', '"')
    s = s.replace('&lt;', '<').replace('&gt;', '>').replace('&amp;', '&')
    return s


def parse_xmq(text: str) -> Node:
    toks = list(xmq_tokens(text))
    pos = 0

    def parse_body(parent: Node, closing: bool):
        nonlocal pos
        while pos < len(toks):
            kind, val = toks[pos]
            if kind == 'op' and val == '}':
                pos += 1
                if closing:
                    return
                raise ValueError('unexpected }')
            if kind != 'str':
                raise ValueError('unexpected %s at token %d' % (val, pos))
            node = Node(val)
            pos += 1
            if pos < len(toks) and toks[pos] == ('op', '('):
                pos += 1
                while toks[pos] != ('op', ')'):
                    k = toks[pos][1]
                    pos += 1
                    if toks[pos] == ('op', '='):
                        pos += 1
                        node.attrs[k] = toks[pos][1]
                        pos += 1
                    else:
                        node.attrs[k] = ''
                pos += 1
            if pos < len(toks) and toks[pos] == ('op', '='):
                pos += 1
                node.value = xmq_unescape(toks[pos][1])
                pos += 1
            elif pos < len(toks) and toks[pos] == ('op', '{'):
                pos += 1
                parse_body(node, True)
            parent.children.append(node)
        if closing:
            raise ValueError('missing }')

    root = Node('root')
    parse_body(root, False)
    return root


# ---------------------------------------------------------------------------
# ixml compiler
# ---------------------------------------------------------------------------
IX_SEQ, IX_ALT, IX_REP, IX_LIT, IX_CLASS, IX_REF, IX_EMPTY = range(7)
IXF_HIDDEN = 1
UNBOUNDED = 0xFFFF


def ixml_tokens(s: str):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
            continue
        if c == '{':
            depth = 1
            i += 1
            while i < n and depth:
                if s[i] == '{':
                    depth += 1
                elif s[i] == '}':
                    depth -= 1
                i += 1
            continue
        if c in "'\"":
            q = c
            j = i + 1
            out = ''
            while j < n:
                if s[j] == q:
                    if j + 1 < n and s[j + 1] == q:
                        out += q
                        j += 2
                        continue
                    break
                out += s[j]
                j += 1
            yield ('lit', out)
            i = j + 1
            continue
        if s.startswith('**', i) or s.startswith('++', i):
            yield ('op', s[i:i + 2])
            i += 2
            continue
        if c in '=:.,;|()[]*+?-@^>~#':
            yield ('op', c)
            i += 1
            continue
        j = i
        while j < n and (s[j].isalnum() or s[j] in '_.-') and not (s[j] in '.-' and (j + 1 >= n or not (s[j + 1].isalnum() or s[j + 1] == '_'))):
            j += 1
        if j == i:
            raise ValueError('bad ixml char %r' % c)
        yield ('name', s[i:j])
        i = j


class IxmlCompiler:
    """Compiles the subset of ixml used by wmbusmeters drivers into node tables."""

    def __init__(self, text: str):
        self.toks = list(ixml_tokens(text))
        self.pos = 0
        self.rules: List[Dict[str, Any]] = []   # {'name','mark','rename','body'}
        self.parse_grammar()

    # ---- parsing into python ASTs ----
    def peek(self, k=0):
        return self.toks[self.pos + k] if self.pos + k < len(self.toks) else (None, None)

    def take(self):
        t = self.toks[self.pos]
        self.pos += 1
        return t

    def expect(self, kind, val=None):
        t = self.take()
        if t[0] != kind or (val is not None and t[1] != val):
            raise ValueError('ixml: expected %s %s got %s' % (kind, val, t))
        return t

    def parse_grammar(self):
        while self.pos < len(self.toks):
            mark = None
            if self.peek() in (('op', '-'), ('op', '@'), ('op', '^')):
                mark = self.take()[1]
            name = self.expect('name')[1]
            rename = None
            if self.peek() == ('op', '>'):
                self.take()
                rename = self.expect('name')[1]
            t = self.take()
            if t not in (('op', '='), ('op', ':')):
                raise ValueError('ixml: expected = after %s' % name)
            body = self.parse_alts()
            self.expect('op', '.')
            self.rules.append({'name': name, 'mark': mark, 'rename': rename, 'body': body})

    def parse_alts(self):
        alts = [self.parse_seq()]
        while self.peek() in (('op', '|'), ('op', ';')):
            self.take()
            alts.append(self.parse_seq())
        return alts[0] if len(alts) == 1 else ('alt', alts)

    def parse_seq(self):
        items = []
        if self.peek() in (('op', '.'), ('op', '|'), ('op', ';'), ('op', ')')):
            return ('seq', [])
        items.append(self.parse_term())
        while self.peek() == ('op', ','):
            self.take()
            items.append(self.parse_term())
        return items[0] if len(items) == 1 else ('seq', items)

    def parse_term(self):
        f = self.parse_factor()
        t = self.peek()
        if t == ('op', '*'):
            self.take()
            return ('rep', f, 0, UNBOUNDED)
        if t == ('op', '+'):
            self.take()
            return ('rep', f, 1, UNBOUNDED)
        if t == ('op', '?'):
            self.take()
            return ('rep', f, 0, 1)
        if t in (('op', '**'), ('op', '++')):
            raise ValueError('ixml: separated repetition not supported')
        return f

    def parse_factor(self):
        t = self.peek()
        mark = None
        if t in (('op', '-'), ('op', '@'), ('op', '^')):
            mark = self.take()[1]
            t = self.peek()
        if t == ('op', '+'):
            self.take()
            lit = self.expect('lit')[1]
            return ('ins', lit)
        if t[0] == 'lit':
            self.take()
            return ('lit', t[1], mark == '-')
        if t == ('op', '['):
            return self.parse_class(mark)
        if t == ('op', '('):
            self.take()
            a = self.parse_alts()
            self.expect('op', ')')
            return a
        if t[0] == 'name':
            self.take()
            return ('ref', t[1], mark)
        raise ValueError('ixml: unexpected %s' % (t,))

    def parse_class(self, mark):
        self.expect('op', '[')
        ranges = []
        while True:
            t = self.take()
            if t == ('op', ']'):
                break
            if t in (('op', ';'), ('op', '|')):
                continue
            if t[0] != 'lit':
                raise ValueError('ixml: unsupported class member %s' % (t,))
            lo = t[1]
            if self.peek() == ('op', '-'):
                self.take()
                hi = self.expect('lit')[1]
                ranges.append((lo, hi))
            else:
                for ch in lo:
                    ranges.append((ch, ch))
        return ('class', tuple(sorted(ranges)), mark == '-')

    # ---- compilation ----
    def compile(self) -> Dict[str, Any]:
        rules = {r['name']: r for r in self.rules}
        # Which rules carry a dvk attribute: rules referencing @X where X>dvk = +'...'
        dvk_of_attr_rule = {}
        for r in self.rules:
            if r['rename'] == 'dvk':
                b = r['body']
                if b[0] == 'ins':
                    dvk_of_attr_rule[r['name']] = b[1]
                elif b[0] == 'seq' and len(b[1]) == 1 and b[1][0][0] == 'ins':
                    dvk_of_attr_rule[r['name']] = b[1][0][1]

        def find_dvk(ast) -> Optional[str]:
            kind = ast[0]
            if kind == 'ref' and ast[2] == '@' and ast[1] in dvk_of_attr_rule:
                return dvk_of_attr_rule[ast[1]]
            if kind in ('seq', 'alt'):
                found = None
                for a in ast[1]:
                    d = find_dvk(a)
                    if d is not None:
                        if found is not None and found != d:
                            raise ValueError('ixml: multiple dvks in one rule')
                        found = d
                return found
            if kind == 'rep':
                return find_dvk(ast[1])
            return None

        rule_dvk = {r['name']: find_dvk(r['body']) for r in self.rules}

        self.nodes: List[Tuple[int, int, int, int, int]] = []
        self.children: List[int] = []
        self.literals = ''
        self.out_rules: List[Dict[str, Any]] = []
        rule_index: Dict[str, int] = {}

        def lit_index(text: str) -> int:
            idx = self.literals.find(text)
            if idx >= 0 and text:
                return idx
            idx = len(self.literals)
            self.literals += text
            return idx

        def add_node(t, flags, a, b, c):
            self.nodes.append((t, flags, a, b, c))
            return len(self.nodes) - 1

        def simplify(ast, stack):
            """Inline non-capturing rules, drop attributes/insertions."""
            kind = ast[0]
            if kind == 'ref':
                name, mark = ast[1], ast[2]
                if mark == '@':
                    return ('empty',)
                if name not in rules:
                    raise ValueError('ixml: unknown rule %s' % name)
                if rule_dvk.get(name):
                    return ('capref', name)
                if name in stack:
                    raise ValueError('ixml: recursive rule %s' % name)
                return simplify(rules[name]['body'], stack + [name])
            if kind == 'seq':
                items = []
                for a in ast[1]:
                    s = simplify(a, stack)
                    if s[0] == 'seq':
                        items.extend(s[1])
                    elif s[0] != 'empty':
                        items.append(s)
                # merge adjacent identical classes into one counted class
                merged = []
                for it in items:
                    if merged and it[0] == 'class' and merged[-1][0] == 'class' and it[1] == merged[-1][1]:
                        prev = merged[-1]
                        merged[-1] = ('class', prev[1], prev[2] + it[2])
                    else:
                        merged.append(it)
                if len(merged) == 1:
                    return merged[0]
                return ('seq', merged)
            if kind == 'alt':
                return ('alt', [simplify(a, stack) for a in ast[1]])
            if kind == 'rep':
                return ('rep', simplify(ast[1], stack), ast[2], ast[3])
            if kind == 'class':
                return ('class', ast[1], 1)
            if kind == 'lit':
                return ast
            if kind == 'ins':
                return ('empty',)
            if kind == 'empty':
                return ast
            raise ValueError('ixml: unhandled %s' % kind)

        def emit(ast) -> int:
            kind = ast[0]
            if kind == 'lit':
                return add_node(IX_LIT, IXF_HIDDEN if ast[2] else 0, lit_index(ast[1]), len(ast[1]), 0)
            if kind == 'class':
                text = ''.join(lo + hi for lo, hi in ast[1])
                return add_node(IX_CLASS, 0, lit_index(text), len(text), ast[2])
            if kind == 'empty':
                return add_node(IX_EMPTY, 0, 0, 0, 0)
            if kind == 'capref':
                return add_node(IX_REF, 0, rule_index[ast[1]], 0, 0)
            if kind in ('seq', 'alt'):
                kids = [emit(a) for a in ast[1]]
                start = len(self.children)
                self.children.extend(kids)
                return add_node(IX_SEQ if kind == 'seq' else IX_ALT, 0, start, len(kids), 0)
            if kind == 'rep':
                kid = emit(ast[1])
                start = len(self.children)
                self.children.append(kid)
                return add_node(IX_REP, 0, start, ast[2], ast[3])
            raise ValueError('ixml: emit %s' % kind)

        start_name = self.rules[0]['name']
        capture_rules = [start_name] + [r['name'] for r in self.rules if rule_dvk.get(r['name']) and r['name'] != start_name]
        for name in capture_rules:
            rule_index[name] = len(rule_index)
            self.out_rules.append({'name': name, 'body': None, 'dvk': rule_dvk.get(name)})
        for name in capture_rules:
            body = simplify(rules[name]['body'], [name])
            self.out_rules[rule_index[name]]['body'] = emit(body)
        return {
            'nodes': self.nodes, 'children': self.children, 'rules': self.out_rules,
            'literals': self.literals, 'start': rule_index[start_name],
        }


# ---------------------------------------------------------------------------
# Driver model
# ---------------------------------------------------------------------------
def parse_range(s: str) -> Tuple[int, int]:
    parts = [p.strip() for p in str(s).split(',')]
    a = int(parts[0], 0)
    b = int(parts[1], 0) if len(parts) > 1 else a
    return (a, b)


def parse_int(s: str) -> int:
    s = str(s).strip()
    return int(s, 0)


def c_strtol16(s: str) -> int:
    """Like C strtol(s, NULL, 16): upstream parses lookup values and masks
    as hex even without a 0x prefix (value = 20 means 0x20)."""
    s = str(s).strip()
    neg = False
    if s[:1] in '+-':
        neg = s[0] == '-'
        s = s[1:]
    if s[:2].lower() == '0x':
        s = s[2:]
    digits = ''
    for ch in s:
        if ch in '0123456789abcdefABCDEF':
            digits += ch
        else:
            break
    v = int(digits, 16) if digits else 0
    v = min(v, (1 << 63) - 1)
    return -v if neg else v


def safe_eval(expr: str) -> float:
    if not re.fullmatch(r'[0-9eE+\-*/(). ]+', expr):
        raise ValueError('bad force_scale %s' % expr)
    return float(eval(expr, {'__builtins__': {}}, {}))


def convert_lookup(lk: Node) -> Dict[str, Any]:
    maps = []
    for m in lk.all('map'):
        name = m.get('name', '')
        if m.get('bit') is not None:
            value = 1 << int(str(m.get('bit')).strip(), 10)
        else:
            value = c_strtol16(m.get('value', '0'))
        test = m.get('test', 'Set')
        tl = test.lower()
        maps.append({'name': name, 'value': value, 'test': 1 if tl == 'notset' else 0})
    mt = lk.get('map_type', 'BitToString')
    return {
        'name': lk.get('name', 'LOOKUP'),
        'type': {'BitToString': 0, 'IndexToString': 1, 'DecimalsToString': 2}[mt],
        'mask': c_strtol16(lk.get('mask_bits', '0')),
        'default': lk.get('default_message', ''),
        'maps': maps,
    }


def convert_field(f: Node, driver: str, warnings: List[str]) -> Dict[str, Any]:
    name = f.get('name', '?')
    quantity = f.get('quantity', 'Text')
    if quantity not in QUANTITIES:
        warnings.append('%s: unknown quantity %s' % (driver, quantity))
        quantity = 'Text'
    du = f.get('display_unit')
    display_unit = UNIT_BY_NAME.get(du) if du else QUANTITY_DEFAULT_UNIT[quantity]
    if display_unit is None:
        warnings.append('%s: unknown display unit %s' % (driver, du))
        display_unit = QUANTITY_DEFAULT_UNIT[quantity]
    attrs = 0
    for a in (f.get('attributes') or '').split(','):
        a = a.strip()
        if a:
            if a not in ATTRS:
                warnings.append('%s: unknown attribute %s' % (driver, a))
            attrs |= ATTRS.get(a, 0)
    vif_scaling = f.get('vif_scaling', 'Auto')
    fu = f.get('force_unit')
    force_unit = 'Unknown'
    if fu:
        force_unit = UNIT_BY_NAME.get(fu, 'Unknown')
        vif_scaling = 'None'
    signed = f.get('dif_signedness', 'Default')
    force_scale = safe_eval(f.get('force_scale')) if f.get('force_scale') else 1.0
    flags = 0
    null_value = 0.0
    if f.get('null_value') is not None:
        flags |= 0x04
        null_value = float(f.get('null_value'))
    rs = f.get('readable_string')
    if rs == 'Normal':
        flags |= 0x10
    elif rs == 'Reversed':
        flags |= 0x20
    is_text = quantity == 'Text'
    mep = f.get('match_entire_payload', 'false') == 'true' and is_text
    mef = f.get('match_entire_frame', 'false') == 'true' and is_text

    # matcher
    matches = f.all('match')
    m = {
        'active': len(matches) > 0, 'difvifkey': None, 'mtype': None, 'vif_range': None, 'index_nr': 1,
        'storage': (0, 0), 'tariff': (0, 0), 'subunit': (0, 0), 'combs': [], 'combs_raw': [],
    }
    for mm in matches:
        if mm.get('difvifkey') is not None:
            m['difvifkey'] = mm.get('difvifkey').upper()
            continue
        mt = mm.get('measurement_type')
        if mt is not None:
            m['mtype'] = mt
        vr = mm.get('vif_range')
        if vr is not None:
            m['vif_range'] = vr
        if mm.get('index_nr') is not None:
            m['index_nr'] = parse_int(mm.get('index_nr'))
        if mm.get('storage_nr') is not None:
            m['storage'] = parse_range(mm.get('storage_nr'))
        if mm.get('tariff_nr') is not None:
            m['tariff'] = parse_range(mm.get('tariff_nr'))
        if mm.get('subunit_nr') is not None:
            m['subunit'] = parse_range(mm.get('subunit_nr'))
        for c in mm.all('add_combinable'):
            if c.value not in COMBINABLE_INDEX:
                warnings.append('%s: unknown combinable %s' % (driver, c.value))
                continue
            m['combs'].append(COMBINABLE_INDEX[c.value])
        for c in mm.all('add_combinable_raw'):
            m['combs_raw'].append(parse_int(c.value) & 0xFFFF)
    if m['active'] and (mep or mef):
        mep = mef = False

    tp = f.get('transform_payload')
    transform = None
    if tp:
        parts = [p.strip() for p in tp.split(',')]
        if parts[0] == 'tpl_aes_cbc_iv' and mep:
            if len(parts) == 4:
                transform = (int(parts[1]), int(parts[2]), int(parts[3]))
            else:
                transform = (int(f.get('payload_offset', '0')), int(f.get('payload_length', '0')),
                             int(f.get('payload_tpl_acc_offset', '0')))
    if mep:
        flags |= 0x01
    if mef:
        flags |= 0x02
    if transform:
        flags |= 0x08

    lookups = [convert_lookup(lk) for lk in f.all('lookup')] if is_text else []

    ixml = None
    if f.get('ixml'):
        try:
            ixml = IxmlCompiler(f.get('ixml')).compile()
        except Exception as e:
            warnings.append('%s: ixml compile failed for %s: %s' % (driver, name, e))

    return {
        'name': name, 'quantity': quantity, 'display_unit': display_unit, 'force_unit': force_unit,
        'attrs': attrs, 'flags': flags, 'vif_scaling': vif_scaling, 'signedness': signed,
        'force_scale': force_scale, 'null_value': null_value, 'match': m,
        'calculate': f.get('calculate'), 'lookups': lookups, 'ixml': ixml,
        'transform': transform,
    }


def load_library(path: str, warnings: List[str]) -> Dict[str, Node]:
    doc = parse_xmq(open(path, encoding='utf-8').read())
    lib = doc.first('library')
    out = {}
    for t in lib.all('template'):
        tid = t.attrs.get('id')
        out[tid] = t
        if t.get('aliases'):
            for a in t.get('aliases').split(','):
                out[a.strip()] = t
    return out


def parse_driver(path: str, library: Dict[str, Node], warnings: List[str]) -> Optional[Dict[str, Any]]:
    doc = parse_xmq(open(path, encoding='utf-8').read())
    d = doc.first('driver')
    if d is None:
        return None
    name = d.get('name')
    detects = []
    det = d.first('detect')
    if det:
        for mvt in det.all('mvt'):
            parts = [p.strip() for p in mvt.value.split(',')]
            if len(parts) != 3:
                warnings.append('%s: bad mvt %s' % (name, mvt.value))
                continue
            m, v, t = parts
            ver = 0xFF if v == '*' else int(v, 16)
            typ = 0xFF if t == '*' else int(t, 16)
            detects.append((mfct_code(m) & 0x7fff, ver, typ))
    cff = []
    cf = d.first('compact_frame_formats')
    if cf:
        for dv in cf.all('difvif'):
            b = bytes.fromhex(dv.value)
            cff.append((b, crc16_en13757(b)))
    keys = []
    dk = d.first('default_keys')
    if dk:
        for k in dk.all('key'):
            keys.append(bytes.fromhex(k.value))
    fields = []
    lib = d.first('library')
    if lib:
        for u in lib.all('use'):
            for part in u.value.split(','):
                ref = part.split('|')[0].strip()
                if not ref:
                    continue
                if ref not in library:
                    warnings.append('%s: unknown library field %s' % (name, ref))
                    continue
                fields.append(convert_field(library[ref], name, warnings))
    flds = d.first('fields')
    if flds:
        for f in flds.all('field'):
            fields.append(convert_field(f, name, warnings))
    flags = 0
    tp = d.get('transform_payload')
    if tp == 'diehl_prios':
        flags |= 0x01
    elif tp == 'try_qundis_decode':
        flags |= 0x02
    elif tp == 'buggy_sanxing_609B':
        flags |= 0x04
    tpl_status = None
    ts = d.first('mfct_tpl_status_bits')
    if ts:
        maps = []
        for m in ts.all('map'):
            if m.get('name') is None or m.get('value') is None or m.get('test') is None:
                continue
            maps.append({'name': m.get('name'), 'value': int(m.get('value'), 0),
                         'test': 0 if m.get('test').lower() == 'set' else 1})
        tpl_status = [{'name': 'TPL_STS', 'type': 0, 'mask': int(ts.get('mask_bits', '0xff'), 0),
                       'default': ts.get('default_message', 'OK'), 'maps': maps}]
    for fl in fields:
        if (fl['attrs'] & ATTRS['INCLUDE_TPL_STATUS']) and not fl['match']['active'] and fl['lookups']:
            tpl_status = fl['lookups']
            break
    mt = d.get('meter_type', 'UnknownMeter')
    if mt not in METER_TYPES:
        warnings.append('%s: unknown meter type %s' % (name, mt))
        mt = 'UnknownMeter'
    return {
        'name': name, 'aliases': d.get('aliases'), 'meter_type': mt,
        'default_fields': d.get('default_fields', ''), 'force_media': d.get('force_media_type'),
        'flags': flags, 'detects': detects, 'fields': fields, 'tpl_status': tpl_status,
        'compact': cff, 'keys': keys, 'tests': parse_tests(d, name),
    }


def parse_tests(d: Node, driver: str) -> List[Dict[str, Any]]:
    out = []
    tests = d.first('tests')
    if not tests:
        return out
    for t in tests.all('test'):
        args = t.get('args')
        telegram = t.get('telegram')
        js = t.get('json')
        if not telegram or not js:
            continue
        parts = (args or '').split()
        name = parts[0] if parts else ''
        key = parts[3] if len(parts) >= 4 else 'NOKEY'
        drv = parts[1] if len(parts) >= 2 else driver
        try:
            expected = json.loads(js)
        except Exception:
            continue
        out.append({'driver': drv, 'name': name, 'key': key, 'telegram': telegram.replace(' ', ''),
                    'expected': expected})
    return out


# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------
def cstr(s: Optional[str]) -> str:
    if s is None:
        return 'nullptr'
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif o < 32 or o > 126:
            for b in ch.encode('utf-8'):
                out.append('\\%03o' % b)
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def fmt_double(v: float) -> str:
    r = repr(float(v))
    if 'e' not in r and '.' not in r and 'inf' not in r:
        r += '.0'
    return r


class Emitter:
    def __init__(self):
        self.lines: List[str] = []
        self.lookup_pool: Dict[str, str] = {}
        self.pool_lines: List[str] = []
        self.counter = 0

    def uid(self, prefix):
        self.counter += 1
        return '%s_%d' % (prefix, self.counter)

    def lookups(self, rules: List[Dict[str, Any]]) -> str:
        key = json.dumps(rules, sort_keys=True)
        if key in self.lookup_pool:
            return self.lookup_pool[key]
        sym = self.uid('LK')
        rule_syms = []
        for r in rules:
            msym = 'nullptr'
            if r['maps']:
                msym = self.uid('LM')
                self.pool_lines.append('static const LookupMap %s[] = {' % msym)
                for m in r['maps']:
                    self.pool_lines.append('    { 0x%XULL, %s, TestBit::%s },' % (
                        m['value'] & 0xFFFFFFFFFFFFFFFF, cstr(m['name']), 'NotSet' if m['test'] else 'Set'))
                self.pool_lines.append('};')
            rule_syms.append('    { %s, MapType::%s, 0x%XULL, %s, %s, %d },' % (
                cstr(r['name']), ['BitToString', 'IndexToString', 'DecimalsToString'][r['type']],
                r['mask'] & 0xFFFFFFFFFFFFFFFF, cstr(r['default']), msym, len(r['maps'])))
        self.pool_lines.append('static const LookupRule %s[] = {' % sym)
        self.pool_lines.extend(rule_syms)
        self.pool_lines.append('};')
        self.lookup_pool[key] = sym
        return sym

    def ixml(self, g: Dict[str, Any]) -> str:
        sym = self.uid('IX')
        L = self.lines
        L.append('static const IxmlNode %s_N[] = {' % sym)
        for (t, fl, a, b, c) in g['nodes']:
            L.append('    { %d, %d, %d, %d, %d },' % (t, fl, a, b, c))
        L.append('};')
        L.append('static const uint16_t %s_C[] = { %s };' % (sym, ', '.join(str(c) for c in g['children']) or '0'))
        L.append('static const IxmlRule %s_R[] = {' % sym)
        for r in g['rules']:
            L.append('    { %s, %d, %s },' % (cstr(r['name']), r['body'], cstr(r['dvk'])))
        L.append('};')
        L.append('static const IxmlGrammar %s = { %s_N, %s_C, %s_R, %s, %d, %d, %d };' % (
            sym, sym, sym, sym, cstr(g['literals']), len(g['nodes']), len(g['rules']), g['start']))
        return sym


def gen_field(em: Emitter, f: Dict[str, Any]) -> str:
    m = f['match']
    flags = []
    if m['active']:
        flags.append('FM_ACTIVE')
    dvk = 'nullptr'
    mtype = 'MeasurementType::Any'
    vr = 'VifRange::Any'
    st, ta, su = m['storage'], m['tariff'], m['subunit']
    if m['active']:
        if m['difvifkey']:
            flags.append('FM_DIFVIFKEY')
            dvk = cstr(m['difvifkey'])
        else:
            if m['mtype'] and m['mtype'] != 'Any':
                flags.append('FM_MTYPE')
                mtype = 'MeasurementType::' + m['mtype']
            if m['vif_range'] and m['vif_range'] != 'Any':
                if m['vif_range'] not in VIF_RANGES:
                    raise ValueError('unknown vif range %s' % m['vif_range'])
                flags.append('FM_VIF_RANGE')
                vr = 'VifRange::' + m['vif_range']
            flags.extend(['FM_STORAGE', 'FM_TARIFF', 'FM_SUBUNIT'])
            if COMBINABLE_INDEX['Any'] in m['combs']:
                flags.append('FM_COMB_ANY')
    combs = 'nullptr'
    if m['combs']:
        combs = em.uid('CB')
        em.lines.append('static const uint8_t %s[] = { %s };' % (combs, ', '.join(str(c) for c in m['combs'])))
    combs_raw = 'nullptr'
    if m['combs_raw']:
        combs_raw = em.uid('CR')
        em.lines.append('static const uint16_t %s[] = { %s };' % (combs_raw, ', '.join('0x%X' % c for c in m['combs_raw'])))
    lk = 'nullptr'
    nlk = 0
    if f['lookups']:
        lk = em.lookups(f['lookups'])
        nlk = len(f['lookups'])
    ix = 'nullptr'
    if f['ixml']:
        ix = '&' + em.ixml(f['ixml'])
    tr = f['transform'] or (-1, -1, -1)
    return ('    { %s, Quantity::%s, Unit::%s, Unit::%s, 0x%04X, 0x%02X, VifScaling::%s, DifSignedness::%s, %s, %s,\n'
            '      { %s, %s, %s, %s, 0, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s, %s },\n'
            '      %s, %s, %d, %s, %d, %d, %d },') % (
        cstr(f['name']), f['quantity'], f['display_unit'], f['force_unit'], f['attrs'], f['flags'],
        f['vif_scaling'], f['signedness'], fmt_double(f['force_scale']), fmt_double(f['null_value']),
        ' | '.join(flags) or '0', dvk, mtype, vr, st[0], st[1], ta[0], ta[1], su[0], su[1],
        m['index_nr'], len(m['combs']), len(m['combs_raw']), combs, combs_raw,
        cstr(f['calculate']), lk, nlk, ix, tr[0], tr[1], tr[2])


def generate(drivers: List[Dict[str, Any]], out_path: str):
    em = Emitter()
    driver_rows = []
    for d in drivers:
        sym = re.sub(r'[^A-Za-z0-9_]', '_', d['name'])
        det = 'nullptr'
        if d['detects']:
            det = 'DET_' + sym
            em.lines.append('static const DriverDetect %s[] = {' % det)
            for (m, v, t) in d['detects']:
                em.lines.append('    { 0x%04X, 0x%02X, 0x%02X },' % (m, v, t))
            em.lines.append('};')
        field_rows = [gen_field(em, f) for f in d['fields']]
        fld = 'nullptr'
        if field_rows:
            fld = 'FLD_' + sym
            em.lines.append('static const FieldDef %s[] = {' % fld)
            em.lines.extend(field_rows)
            em.lines.append('};')
        ts = 'nullptr'
        nts = 0
        if d['tpl_status']:
            ts = em.lookups(d['tpl_status'])
            nts = len(d['tpl_status'])
        cf = 'nullptr'
        if d['compact']:
            cf = 'CF_' + sym
            for i, (b, sig) in enumerate(d['compact']):
                em.lines.append('static const uint8_t %s_%d[] = { %s };' % (cf, i, ', '.join('0x%02X' % x for x in b)))
            em.lines.append('static const CompactFormat %s[] = {' % cf)
            for i, (b, sig) in enumerate(d['compact']):
                em.lines.append('    { %s_%d, %d, 0x%04X },' % (cf, i, len(b), sig))
            em.lines.append('};')
        keys = 'nullptr'
        if d['keys']:
            keys = 'KEYS_' + sym
            em.lines.append('static const uint8_t %s[][16] = {' % keys)
            for k in d['keys']:
                em.lines.append('    { %s },' % ', '.join('0x%02X' % x for x in k))
            em.lines.append('};')
        driver_rows.append('    { %s, %s, MeterType::%s, %s, %s, 0x%02X, %s, %d, %s, %d, %s, %d, %s, %d, %s, %d },' % (
            cstr(d['name']), cstr(d['aliases']), d['meter_type'], cstr(d['default_fields']), cstr(d['force_media']),
            d['flags'], det, len(d['detects']), fld, len(d['fields']), ts, nts, cf, len(d['compact']),
            keys, len(d['keys'])))

    out = ['// GENERATED by tools/generate_drivers.py from the wmbusmeters driver sources. DO NOT EDIT.',
           '// wmbusmeters is Copyright (C) Fredrik Öhrström and contributors (gpl-3.0-or-later).',
           '#include "wmbus/driver_table.h"',
           '#include "wmbus/ixml.h"',
           '',
           'namespace wmb {',
           '']
    out += em.pool_lines
    out.append('')
    out += em.lines
    out.append('')
    out.append('const DriverDef DRIVERS[] = {')
    out += driver_rows
    out.append('};')
    out.append('const size_t DRIVERS_LEN = %d;' % len(driver_rows))
    out.append('')
    out.append('} // namespace wmb')
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(out) + '\n')
    print('Generated %s (%d drivers)' % (out_path, len(driver_rows)))


def generate_manufacturers(src_path: str, out_path: str):
    text = open(src_path, encoding='utf-8', errors='replace').read()
    rows = []
    for m in re.finditer(r"X\((\w\w\w),MANFCODE\('(\w)','(\w)','(\w)'\),\"([^\"]*)\"\)", text):
        code = mfct_code(m.group(2) + m.group(3) + m.group(4))
        name = m.group(5)
        # Keep names short for the small screen: drop the ", Country" suffix.
        rows.append((code, name))
    rows.sort()
    out = ['// GENERATED by tools/generate_drivers.py from wmbusmeters src/manufacturers.h. DO NOT EDIT.',
           '#include "wmbus/driver_table.h"', '', 'namespace wmb {', '',
           'struct MfctName { uint16_t code; const char* name; };',
           'static const MfctName MFCT_NAMES[] = {']
    for code, name in rows:
        out.append('    { 0x%04X, %s },' % (code, cstr(name)))
    out += ['};', '',
            'const char* manufacturer_name(uint16_t mfct) {',
            '    mfct &= 0x7fff;',
            '    size_t lo = 0, hi = sizeof(MFCT_NAMES) / sizeof(MFCT_NAMES[0]);',
            '    while (lo < hi) {',
            '        size_t mid = (lo + hi) / 2;',
            '        if (MFCT_NAMES[mid].code == mfct) return MFCT_NAMES[mid].name;',
            '        if (MFCT_NAMES[mid].code < mfct) lo = mid + 1; else hi = mid;',
            '    }',
            '    return nullptr;',
            '}', '', '} // namespace wmb']
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(out) + '\n')
    print('Generated %s (%d manufacturers)' % (out_path, len(rows)))


def generate_tests(drivers: List[Dict[str, Any]], out_path: str):
    lines = ['// GENERATED by tools/generate_drivers.py from the wmbusmeters driver tests. DO NOT EDIT.',
             '#pragma once', '',
             'struct DriverTestVector {',
             '    const char* driver;',
             '    const char* name;',
             '    const char* key;',
             '    const char* telegram;',
             '    const char* expected;',
             '};', '',
             'static const DriverTestVector DRIVER_TEST_VECTORS[] = {']
    n = 0
    for d in drivers:
        for t in d['tests']:
            lines.append('    { %s, %s, %s, %s, %s },' % (
                cstr(t['driver']), cstr(t['name']), cstr(t['key']), cstr(t['telegram']),
                cstr(json.dumps(t['expected'], separators=(',', ':'), ensure_ascii=False))))
            n += 1
    lines += ['};', 'static const unsigned DRIVER_TEST_VECTORS_LEN = %d;' % n]
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')
    print('Generated %s (%d vectors)' % (out_path, n))


def main():
    ap = argparse.ArgumentParser(description='wM-Buster ADV driver generator')
    ap.add_argument('wmbusmeters', nargs='?', default='.upstream/wmbusmeters')
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    up = args.wmbusmeters
    src = os.path.join(up, 'drivers', 'src')
    if not os.path.isdir(src):
        print('error: %s not found (clone https://github.com/wmbusmeters/wmbusmeters there)' % src)
        sys.exit(1)
    warnings: List[str] = []
    library = load_library(os.path.join(up, 'drivers', 'library.xmq'), warnings)
    drivers = []
    for fn in sorted(os.listdir(src)):
        if not fn.endswith('.xmq'):
            continue
        try:
            d = parse_driver(os.path.join(src, fn), library, warnings)
        except Exception as e:
            warnings.append('%s: parse failed: %s' % (fn, e))
            continue
        if d:
            drivers.append(d)
    for w in warnings:
        print('WARN', w)
    generate(drivers, os.path.join(root, 'lib', 'wmbus', 'src', 'drivers_generated.cpp'))
    generate_manufacturers(os.path.join(up, 'src', 'manufacturers.h'),
                           os.path.join(root, 'lib', 'wmbus', 'src', 'manufacturers_generated.cpp'))
    generate_tests(drivers, os.path.join(root, 'test', 'vectors', 'driver_tests.h'))


if __name__ == '__main__':
    main()
