# Roadmap

LocalCode's direction is a practical native workspace for local coding models:
use your own model, inspect its actions and review the evidence behind the
result. These are areas of intended work, not a delivery schedule.

## Finish 1.0 acceptance

Complete the remaining installed interactive checks: Ling import/load/chat,
switching back to Qwen, stopping an active task, conversation and file persistence
after restart, and small-window usability. Publish the observed results in the
[release status](docs/RELEASE_STATUS_1.0.md) before calling a release Stable.

## Compatibility and everyday usability

Improve model/runtime compatibility reporting, setup errors, memory guidance,
conversation management and recovery. Keep custom runtimes available while
making the included default easy to use. Further platform support needs real
build, runtime and packaging evidence; the current release targets Windows x64.

## Runtime performance, measured later

Evaluate possible llama.cpp optimizations with repeatable measurements on
specified hardware and models. The current bundle is upstream b10655;
**no custom optimization or performance gain is claimed**. Compatibility and
correctness must remain visible alongside speed or memory results.

## Stronger execution boundaries

Investigate OS-level confinement for command processes and their descendants.
This is future work. Current commands run with the user's access, subject to
application policy and approvals; those controls do not provide an OS sandbox.

## More useful evidence

Improve how task criteria, actual command results and file changes are presented.
Keep unsupported claims visible and verification scoped to the checks that
really ran. Model-native capabilities and ordinary source inspection remain
the basis of the workflow.
