`run_tests.py` is the end-to-end suite (registered with CTest as `end_to_end`).
`gen/pagergen.py` builds synthetic POCSAG/FLEX audio from the public protocol
descriptions; it contains no upstream PDW code, so it is an independent check.
