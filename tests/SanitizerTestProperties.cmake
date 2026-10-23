# Keep the full 8 MiB boundary and byte-for-byte tail checks under sanitizers.
# Instrumented CI runs took 175 and 258 seconds, then exceeded the ordinary
# 300-second allowance on another runner. Other cases retain their timeouts.
set_tests_properties(
    "complete source at the edit ceiling remains editable with its tail intact"
    PROPERTIES TIMEOUT 600)
