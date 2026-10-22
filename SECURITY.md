# Security policy

LocalCode can read files, edit a workspace and run native commands. It is a
coding assistant with application-level checks, **not an operating-system
sandbox**. Allowed commands run with your account's filesystem and network
access. Review commands and changes, and keep backups of important work.

## Report a vulnerability privately

Use [GitHub's private vulnerability report](https://github.com/Polterpup2008/LocalCode/security/advisories/new)
for a suspected security issue. Include the LocalCode version or commit, Windows
version, a minimal reproduction, the expected restriction and the observed
impact. Do not publish working exploit details in a public issue while a report
is being assessed.

Remove secrets, private source code, conversation content and identifying paths
from attached logs or screenshots. A response or fix timeline is not guaranteed.

## Useful reports

- A file tool writing outside its permitted roots, including junction, symlink
  or path-validation failures.
- A command-policy or approval bypass that runs an operation the application
  should have refused or presented for confirmation.
- An untrusted download bypassing the application's source or approval checks.
- Unintended disclosure of credentials or other private data through application
  behaviour.
- Model or document content causing a reproducible bypass of an application
  restriction.

Please report suspected boundary failures even if the exact cause is unclear.

## Known boundaries

- Allowed programs and their descendants are not confined to the workspace by
  an OS boundary. A permitted build script can act with your account's access.
- Read access outside the workspace is enabled by default. Settings can restrict
  it; web access is also configurable.
- Prompt injection is not solved. Content from files or the web can steer a
  model, including toward actions that look legitimate.
- Cancellation does not undo completed file edits or command effects.
- The Windows RC2 package is unsigned.

See the [security model](docs/SECURITY.md), [known limitations](docs/KNOWN_LIMITATIONS_1.0.md)
and [release status](docs/RELEASE_STATUS_1.0.md) for details. These checks do not
establish that arbitrary model-generated commands are safe.

## Supported releases

The current published version is **1.0.0-rc.2**, a Windows x64 release candidate.
Security fixes target the latest release or development branch; older versions
do not have a separate maintenance commitment.
