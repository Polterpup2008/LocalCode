## What this changes

<!-- One or two sentences. What is different afterwards? -->

## Why

<!-- The problem being solved. Link an issue if there is one. -->

## How it was verified

<!--
Be specific. "Builds and the tests pass" with the command you ran beats
"should work". If you could not verify something, say so - that is more useful
than an unqualified claim.
-->

- [ ] `ctest --preset windows-core` (or `linux-core`) passes
- [ ] New behaviour has a test that fails without the change
- [ ] Ran the desktop application and exercised the changed path

Commands run:

```
```

## Checklist

- [ ] `localcode_core` still does not link Qt
- [ ] Any new tool returns a small result and attaches evidence
- [ ] No security rule was loosened; if the policy changed, the description
      below says exactly what is now permitted that was not before
- [ ] Nothing is emulated silently — a missing model capability is absent from
      the UI rather than faked
- [ ] Documentation updated if behaviour changed

## Security impact

<!-- "None" is a fine answer. If the path or command policy changed, spell out
     the new surface. -->

None.
