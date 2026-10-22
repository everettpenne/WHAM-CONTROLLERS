"""dangerous_commands.py -- which controller commands are dangerous (docs/device_identity_and_host_service.md).

A command is dangerous when sending it starts or enables real output, switches a safety interlock or protection off,
forces or clears a fault, reflashes or resets the controller, or erases the board's identity. The host service
(wham_hub.py) logs these, shows them to every connected client, and can hold them until confirmed.

The list is hand-written and matches commands the way the firmware does: per ':'-level, the leading run of UPPERCASE
letters of a pattern is the mandatory short form and the whole token is the long form (scpi_parser.c's
scpi_token_match()), case-insensitive, with no partial long forms. Queries (a trailing '?') are never dangerous.
Turning an output OFF, or an interlock ON, is the safe direction and is never listed; a malformed or missing
argument where the direction matters counts as dangerous (the firmware will refuse it, but the intent was there).

wham_console.py has an older, narrower _is_dangerous() (FIRE, BOOT, SOURce:RUN, SHOT:STARt, SOURce:ENAble on); this list is a
superset of it (tests/test_dangerous_commands.py checks that, and that every pattern here exists in a real command table).
When a command is added to either product that starts output or weakens a protection, add it here.
"""
import re
from collections import namedtuple

Danger = namedtuple("Danger", "category pattern")

ANY = "any"            # every non-query use
ON = "on"              # the last argument is nonzero (or missing/unparseable)
OFF = "off"            # the last argument is zero (or missing/unparseable)

# (pattern as in command_table.c, when it applies, category)
RULES = [
    # starts or enables real output
    ("FIRE",                    ANY, "output"),
    ("SOURce:RUN",              ANY, "output"),
    ("SHOT:STARt",              ANY, "output"),
    ("EXTernal:TRIGger",        ON,  "output"),     # a rising edge on PF15 then starts the shot while ARMED (both products)
    ("SOURce:ENAble",           ON,  "output"),     # XREX: SOURce:ENAble <ch> <0|1>
    ("XREX:CHANnel:ENAOut",     ON,  "output"),     # XREX: a Transrex unit's enable line
    ("XREX:CHANnel:CONTactOut", ON,  "output"),     # XREX: its contactor
    ("PHASe:ENAble",            ON,  "output"),     # HBRIDGE: PHASe:ENAble UW 1
    ("PHASe:SELect",            ANY, "output"),     # HBRIDGE: selects which phases drive
    ("DAC:OUTPut",              ON,  "output"),     # HBRIDGE: an analog output voltage
    ("GPOut:ENAble",            ANY, "output"),     # board enable outputs (either direction)
    ("PWMAlt:ENAble",           ANY, "output"),
    ("DIAGnostic:GPOut09",      ON,  "output"),
    ("DIAGnostic:GPOut10",      ON,  "output"),
    ("DIAGnostic:GPOut11",      ON,  "output"),
    ("DIAGnostic:GPOut12",      ON,  "output"),
    # makes the controller ready to fire
    ("ARM",                     ANY, "arm"),
    # switches a protection off, or changes how faults are read or limited
    ("EXTernal:ENAble",         OFF, "interlock"),  # XREX: the PF13 interlock off
    ("DEBUG:FAULT:BYPASS",      ON,  "interlock"),  # XREX
    ("CONFig:FaultPolarity:WATER",   ANY, "interlock"),
    ("CONFig:FaultPolarity:TEMP",    ANY, "interlock"),
    ("CONFig:FaultPolarity:ENERPRO", ANY, "interlock"),
    ("CONFig:FaultPolarity:OCP",     ANY, "interlock"),
    ("FAULT:CLEar",             ANY, "interlock"),  # releases a latched fault
    ("CONFig:MaxCarrierHz",     ANY, "limits"),
    ("CONFig:MAXFREQHz",        ANY, "limits"),
    ("CONFig:MAXCURRent",       ANY, "limits"),
    ("CONFig:MAXVolts",         ANY, "limits"),
    ("CONFig:SLEWRate",         ANY, "limits"),
    ("CONFig:FaultRampTime",    ANY, "limits"),
    # forces a fault
    ("GENERAL:TEST:FAULT",      ANY, "fault-inject"),
    ("OCP:TEST:FAULT",          ANY, "fault-inject"),
    # reflashes or resets the controller (FWUPdate:DATA/END/ABORt/STATus are part of an update, not announced)
    ("BOOT",                    ANY, "firmware"),
    ("FWUPdate:BEGin",          ANY, "firmware"),
    ("FWUPdate:SWAP",           ANY, "firmware"),
    ("FWUPdate:ROLLback",       ANY, "firmware"),
    # the board's identity
    ("SYS:SERial",              ANY, "identity"),
    ("SYS:ID:CLEar",            ANY, "identity"),
]


def token_match(pattern: str, text: str) -> bool:
    """One ':'-level, as scpi_token_match() in the firmware: the short form is the pattern's leading run of
    uppercase letters, the long form is the whole token; case-insensitive; nothing in between."""
    short = 0
    while short < len(pattern) and pattern[short].isupper() and pattern[short].isascii():
        short += 1
    t = text.upper()
    return t == pattern.upper() or (len(t) == short and t == pattern[:short].upper())


def path_matches(pattern: str, path: str) -> bool:
    p_levels, c_levels = pattern.split(":"), path.lstrip(":").split(":")
    return len(p_levels) == len(c_levels) and all(token_match(p, c) for p, c in zip(p_levels, c_levels))


def _number(token):
    try:
        return float(token)
    except ValueError:
        return None


def _applies(when, args):
    if when == ANY:
        return True
    value = _number(args[-1]) if args else None
    if value is None:
        return True                      # missing or not a number: treated as the dangerous reading
    return (value != 0.0) if when == ON else (value == 0.0)


def classify(command: str):
    """The Danger (category, pattern) of one command line, or None."""
    parts = command.strip().split()
    if not parts or parts[0].endswith("?"):
        return None
    path, args = parts[0], parts[1:]
    for pattern, when, category in RULES:
        if path_matches(pattern, path) and _applies(when, args):
            return Danger(category, pattern)
    return None


def is_dangerous(command: str) -> bool:
    return classify(command) is not None
