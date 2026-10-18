// Stops a build unless CHANGELOG.md (at the repo root) has a section for the version in package.json with at least one
// "- " bullet. The release script puts that section on the GitHub release.
//
//   node scripts/check-changelog.ts
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { notesFor } from "@flare/release/changelog";

const app = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const { version }: { version: string } = JSON.parse(readFileSync(resolve(app, "package.json"), "utf8"));

const notes = notesFor(readFileSync(resolve(app, "..", "CHANGELOG.md"), "utf8"), version);
if (!notes) fail(`CHANGELOG.md has no "## ${version}" section.`);
if (notes.length === 0) fail(`The "## ${version}" section of CHANGELOG.md has no "- " bullet points.`);
console.log(`Release notes for ${version}:\n\n${notes.map((item) => `- ${item}`).join("\n")}\n`);

function fail(problem: string): never {
  console.error(`\n${problem}\nAdd release notes for ${version} (see docs/release-notes.md) before building.\n`);
  process.exit(1);
}
