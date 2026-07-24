#!/usr/bin/env python3

"""Generate a serialisation .dsl by expanding FIX messages from a data dictionary.

Thin script wrapper (like generate_cpp_from_dsl.py and generate_fix_dictionary.py) so the
build can invoke the generator without setting PYTHONPATH; the real logic lives in the
dd_to_dsl package. See `python -m dd_to_dsl --help` for the arguments.
"""

import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_PYTHON_DIR = os.path.join(SCRIPT_DIR, "..")
sys.path.insert(0, PROJECT_PYTHON_DIR)

# pylint: disable=wrong-import-position
from dd_to_dsl.__main__ import main

if __name__ == "__main__":
    sys.exit(main())
