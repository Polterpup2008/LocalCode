---
id: secrets-and-credentials
name: Handle secrets without leaking them
description: What to do when a task involves API keys, tokens or passwords, and how to avoid writing one into a file, a log or a commit.
tags: [security, secrets, git, discipline]
triggers: [api key, token, password, secret, credential, .env, auth, private key]
priority: 85
---

# Handle secrets without leaking them

A secret that reaches a repository, a log file or a tool result has to be treated
as compromised and rotated. Prevention is the whole job.

## When this applies

The task mentions an API key, a token, a password, a connection string, a
certificate, or a `.env` file - whether or not it is the point of the task.

## Rules

1. **Never write a real secret into a tracked file.** Configuration goes in a
   file that is git-ignored, or in an environment variable. If a project needs
   `.env`, create `.env.example` with placeholder values and add `.env` to
   `.gitignore`.

2. **Never echo a secret into output.** Do not `read_file` a `.env` and paste it
   into your answer, do not print an environment variable that holds a token, and
   do not include one in a command you run - arguments end up in logs and in the
   command history LocalCode keeps.

3. **Never commit one, even to fix it.** If you find a secret already committed,
   stop and tell the user. Removing it in a new commit does not remove it from
   history, and rewriting history is theirs to decide.

4. **Redact when quoting.** If you must show a line that contains one, replace
   the value: `API_KEY=sk-...redacted`. Keep enough to identify which key, not
   enough to use it.

## Recognising one

Treat as secret: anything named `*_KEY`, `*_TOKEN`, `*_SECRET`, `*_PASSWORD`,
`AUTHORIZATION`; strings that look like `sk-`, `ghp_`, `AKIA`, `xox`-prefixed
tokens; PEM blocks; and connection strings with a password field.

A value in a test fixture that is obviously fake (`password123`,
`test-token`) is not a secret, but do not assume - if it looks real, treat it as
real.

## If you are asked to add authentication

Write the code that *reads* the credential from the environment or a
configuration file. Leave the value itself to the user, and say clearly where
they need to put it. Do not invent a placeholder that looks real enough to be
mistaken for one.

## Procedure

1. Before writing any file, check whether the content contains a credential.
2. Before including output in your answer, scan it for the patterns above.
3. If the task requires a secret you do not have, say so and name the variable
   the user must set - do not guess or fabricate a value.
4. If you discover a leaked secret, report it as a finding and stop.

## Failure modes

- **Pasting a `.env` into the conversation to explain a configuration problem.**
- **Hardcoding a key "temporarily" to get something working.** It stays.
- **Adding `.env` to the repository because the build needed it.**
- **Quietly deleting a committed secret.** The user needs to know so they can
  rotate it; silence is the harm here.
