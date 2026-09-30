# Contributing to Resource Entitlement

Thanks for your interest in Resource Entitlement. This guide explains how to
build and test the project, what a pull request needs to contain, and how
contributions are licensed. Bug reports, documentation fixes, and small patches
are just as welcome as large features.

## Before you start

* For anything larger than a bug fix, open an issue first so we can agree on the
  approach before you invest time in an implementation.
* Search existing issues and pull requests to avoid duplicating work.
* Be respectful and constructive in issues, reviews, and discussions. Critique
  the code, not the person.

## Prerequisites

* A C++20 compiler: Visual Studio 2022 (MSVC 19.3x or newer), GCC 12 or newer,
  or Clang 15 or newer.
* CMake 3.25 or newer.
* A build tool for your platform: Visual Studio, Ninja, or Make.

## Building

Configure a build directory and build it. Use a separate directory per
configuration so that Debug and Release artifacts never mix:

```sh
# Debug
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --config Debug

# Release
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --config Release
```

The project is built with strict warnings enabled. On MSVC that means `/W4`,
`/WX`, and `/permissive-`; the equivalent strict settings are used for GCC and
Clang. Warnings are errors, so a warning-free build in **both** Debug and
Release is expected. Do not silence a warning with a blanket pragma or a
compiler flag; fix the underlying issue.

## Testing

Tests are registered with CTest and are built alongside the library:

```sh
ctest --test-dir build/debug -C Debug --output-on-failure
ctest --test-dir build/release -C Release --output-on-failure
```

**All tests must pass before you open a pull request.** Run the full suite in
Debug and Release, and make sure your change is covered by tests: new behavior
needs new test cases, and bug fixes need a regression test that fails without
the fix. If a test is flaky rather than broken, say so in the pull request
instead of disabling it.

## Code quality expectations

* **No placeholders.** Pull requests must not contain `TODO` comments,
  unimplemented stubs, commented-out code, or dead code paths. If the work is
  not finished, keep it out of the pull request. If a genuine follow-up is
  needed, open an issue and reference it in the description.
* **Strong typing for identities and generations.** Do not pass bare integers,
  strings, or pointers where an identity is meant. Wrap identifiers, handles,
  and generation counters in distinct types so unrelated values cannot be
  swapped by accident and stale handles cannot be reused. Prefer scoped enums
  and explicit newtypes over `bool` flags and magic numbers.
* **Treat external input as untrusted and bounded.** Every value that comes from
  outside the library — files, network data, caller-supplied buffers, command
  line arguments — must be validated before use. Enforce explicit limits on
  sizes, counts, and ranges, check for overflow, handle malformed or truncated
  data without crashing, and never trust a length or offset supplied by the
  caller. Allocation and recursion driven by input must be bounded.
* **Deterministic behavior.** The same inputs must produce the same outputs.
  Do not let unordered container iteration order, wall-clock time, address-based
  hashing, uninitialized memory, or locale-dependent formatting leak into
  observable results. Seed any randomness explicitly.
* **Own resources correctly.** Use RAII and the standard smart pointers; no raw
  owning pointers, no manual `new`/`delete` in new code, and no resource leaks
  on error paths.
* **Keep the dependency surface small.** Do not add a new third-party runtime
  dependency without justification. Propose it in an issue first and explain
  what it buys, what it costs, and why the standard library is not enough. Any
  accepted dependency must be license-compatible with Apache-2.0.
* **Stay consistent with the code around you.** Match the existing naming,
  formatting, and file layout, keep public headers minimal, and keep the diff
  focused on the change being made.
* **Document the interface.** New public API needs a short comment describing
  what it does, what it requires, and what it guarantees.

## Commit messages

* Use an imperative, capitalized summary line of about 72 characters or fewer,
  with no trailing period: `Add bounded parsing for entitlement tokens`.
* Leave a blank line, then use the body to explain what changed and, more
  importantly, why. Wrap the body at 72 columns.
* Reference the issue a change closes, for example `Fixes #42`.
* Keep one logical change per commit so the history stays bisectable. Rebase on
  the target branch rather than merging it into your topic branch.
* Do not add attribution trailers. `Co-authored-by:` trailers and generated-by
  or tool-attribution lines are not used in this repository; a commit should
  list only the people who actually wrote the change.

## Opening a pull request

1. Fork the repository and work on a topic branch with a descriptive name.
2. Make sure the build is warning-free and the full test suite passes in Debug
   and Release.
3. Fill in the pull request description: what the change does, why it is needed,
   how it was tested, and any behavior or compatibility impact.
4. Keep the pull request focused. Unrelated cleanups belong in their own pull
   request, and large changes are easier to review when split into a few
   logically ordered commits.
5. Respond to review comments with code or with reasoning. Reviewers may ask for
   changes; maintainers make the final call on what gets merged.

## Reporting bugs

Open an issue and include:

* A clear title and a description of the expected versus the actual behavior.
* The exact commit or release you tested.
* Your operating system, compiler and version, and build configuration
  (Debug or Release).
* The exact commands you ran and their output.
* A minimal, self-contained reproduction — the smallest input and the smallest
  program or test that still shows the problem. Please reduce it yourself if
  you can. Large pasted logs are less useful than a small reproducer.

## Reporting security issues

Do not report security vulnerabilities in a public issue, and do not include
exploit details, proof-of-concept code, or crash dumps that reveal them in a
public thread. Use the repository's **Security** tab and choose
**Report a vulnerability** to open a private advisory, which is visible only to
the maintainers. Include the affected version or commit, the impact, the steps
to reproduce, and any suggested fix or mitigation. If you cannot use that
channel, open a minimal public issue asking for a private contact channel
without disclosing any details of the problem, and a maintainer will follow up.
We will acknowledge your report privately and keep you updated on the fix and
on any coordinated disclosure.

## Licensing of contributions

Resource Entitlement is released under the Apache License, Version 2.0. See
[LICENSE](LICENSE) and [NOTICE](NOTICE).

Contributions are accepted under the same license — inbound equals outbound:

* By submitting a pull request, patch, or any other contribution, you agree that
  your contribution is licensed to the project and to everyone who receives it
  under the Apache License, Version 2.0, with no additional terms or conditions.
  This mirrors Section 5 of the license: unless you explicitly state otherwise,
  any contribution intentionally submitted for inclusion in the work is under
  the terms of the Apache License, Version 2.0.
* **No contributor license agreement (CLA) is required, and you do not assign
  your copyright.** You keep the copyright in your contribution; you simply
  license it under Apache-2.0 like the rest of the project.
* Only submit work you have the right to submit. If your employer owns the
  intellectual property you create at work, make sure you have permission
  before contributing, and let us know in the pull request if that is relevant.
* Do not copy code from another project unless its license is compatible with
  Apache-2.0 and you preserve the required notices. Say where the code came
  from in the pull request description.
* You do not need to add a license header to new files; the repository-level
  [LICENSE](LICENSE) covers the project. Leave existing copyright and license
  notices in place.
