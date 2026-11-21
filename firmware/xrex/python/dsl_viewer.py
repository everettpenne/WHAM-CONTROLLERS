#!/usr/bin/env python3
"""dsl_viewer.py moved to host/dsl_viewer.py (2026-11-20). This stub runs it from there with the same arguments, so old commands keep working."""
import os
import runpy
import sys

_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "host", "dsl_viewer.py")
if not os.path.exists(_TOOL):
    sys.exit("dsl_viewer.py now lives in host/ of the WHAM-CONTROLLERS repository, and this copy cannot find it: run host/dsl_viewer.py")
sys.argv[0] = _TOOL
runpy.run_path(_TOOL, run_name="__main__")
