#!/usr/bin/env python3
"""Stands in for rtl_fm in tests: writes the raw s16le audio of $FAKE_RTL_PCM to stdout, then exits."""
import os
import shutil
import sys

with open(os.environ["FAKE_RTL_PCM"], "rb") as f:
    shutil.copyfileobj(f, sys.stdout.buffer)
sys.stdout.buffer.flush()
