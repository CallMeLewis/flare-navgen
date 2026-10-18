# Writing release notes

Every version's notes are a section of [`CHANGELOG.md`](../CHANGELOG.md), newest at the top. Despite its name, it holds
release notes for the people using Flare Navgen, not a record of every change: that record is the git history.

```
## 1.2.0 - 2026-11-14

- New ...
- Fixed ...
```

- The heading is `## ` and the version, then ` - ` and the release date (YYYY-MM-DD). `pnpm bump <version>` (in
  `app/`) adds it.
- The build stops if the version in `app/package.json` has no section, or the section has no bullets.
- The GitHub release shows the same notes, followed by a link to every commit since the previous release.

## Format

Only the lines that start with `- ` count, as plain text in a bulleted list:

- One bullet per line, starting with `- `. Headings, blank lines and wrapped lines are dropped.
- No Markdown inside a bullet: `**bold**`, `` `code` `` and links appear as the raw characters.
- Order the bullets: New, then Improved, then Fixed.

## Writing them

Release notes are for server owners and map makers using the app, not for the people developing it.

- Describe what changed from the user's point of view: what they can now do, what behaves differently, or what problem
  is gone. Lead with the result, not how it was built.
- Say where to find anything new: on the Generate page, under Advanced, in Settings.
- Use plain language and the app's own words (button labels, setting names). No developer terms, internal names,
  command-line flags, or file, class or variable names.
- Keep each bullet to one sentence where practical, with a second only when it adds important context.
- Be specific and factual: no "various improvements" or "bug fixes" when you know what actually changed, and no
  marketing language.
- Leave out anything users can't notice: refactoring, dependency updates, tests, CI, build and packaging changes.
