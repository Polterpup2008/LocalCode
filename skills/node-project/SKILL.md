---
id: node-project
name: Work in a JavaScript or TypeScript project
description: How to find the right package manager and scripts, and how to read the errors these toolchains produce.
tags: [javascript, typescript, node, npm, build, tests]
triggers: [npm, node, yarn, pnpm, package.json, typescript, tsc, jest, vitest, eslint]
priority: 68
---

# Work in a JavaScript or TypeScript project

Two things go wrong most often: using the wrong package manager, and inventing a
command when the project already defines one.

## When this applies

The workspace contains `package.json`.

## Establish the package manager from the lockfile

The lockfile decides, not preference:

- `package-lock.json` → **npm**
- `yarn.lock` → **yarn**
- `pnpm-lock.yaml` → **pnpm**
- `bun.lockb` → **bun**

Using a different one rewrites the lockfile and produces an enormous, unwanted
diff. If several lockfiles are present, say so - that is a problem in the repo,
not a choice for you to make silently.

Install with the command that respects the lockfile: `npm ci` when it exists and
matches `package.json`, otherwise `npm install`.

## Read `scripts` before running anything

`package.json` has a `scripts` block. Use what is there - `npm run build`,
`npm test`, `npm run lint` - rather than calling `tsc` or `jest` directly. The
script usually carries flags and configuration the bare tool will not pick up.

If no `test` script exists, say so instead of guessing a runner.

## TypeScript errors

`tsc` errors are precise; read the whole message including the "Type X is not
assignable to type Y" expansion, which names the exact incompatible property.

- **Do not reach for `any`.** It silences the checker and moves the failure to
  runtime. If a type is genuinely unknown, `unknown` forces you to narrow it.
- **Do not add `@ts-ignore`.** If you cannot fix it, say why.
- `strict` mode differences explain most surprises about `null` and `undefined`.
  Check `tsconfig.json` before arguing with the compiler.

Type-checking is separate from bundling: a build tool such as esbuild or Vite may
strip types without checking them, so a green build does not mean `tsc` passes.
Run the project's type-check script too.

## Node version

`package.json` may declare `engines.node`, and the repo may have `.nvmrc`. A
mismatch produces failures that look like code bugs - unsupported syntax, missing
globals. Check `node --version` against those before debugging further.

## Failure modes

- **Running `npm install` in a pnpm project**, replacing the lockfile.
- **Calling `jest` directly** when the script sets a config path.
- **Adding `any` to make the build pass.**
- **Adding a dependency for something the standard library does.**
- **Reporting success from a bundler build that never type-checked.**
