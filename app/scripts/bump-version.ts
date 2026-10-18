// Sets a new version everywhere it's written (app/package.json, which tauri.conf.json reads, the app's and the engine's
// Cargo.toml and Cargo.lock, and the C++ engine's CMakeLists.txt) and adds a dated section for it at the top of
// CHANGELOG.md, ready for its release notes. The build stops until the section has at least one bullet.
//
//   node scripts/bump-version.ts <version>      (pnpm bump 1.2.0, or 1.2.0-beta.1 for a beta)
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { compareVersions, parseChangelog, versionProblem } from "@flare/release/changelog";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const path = (file: string) => resolve(root, file);

const version = process.argv[2];
if (!version) fail("Give the new version, such as 1.2.0.");
const problem = versionProblem(version, "Flare Navgen");
if (problem) fail(problem);

const current: string = JSON.parse(readFileSync(path("app/package.json"), "utf8")).version;
if (compareVersions(version, current) <= 0) fail(`${version} isn't newer than the current version, ${current}.`);

// Each file is edited as text, so the rest of it stays exactly as it was. Nothing is written until every file has
// been found to have its version where expected.
const name = readFileSync(path("app/src-tauri/Cargo.toml"), "utf8").match(/^name\s*=\s*"([^"]+)"/m)?.[1];
if (!name) fail("Couldn't find the package name in app/src-tauri/Cargo.toml.");
const edits = [
  edited("app/package.json", /^(\s*"version":\s*")[^"]+(")/m),
  edited("app/src-tauri/Cargo.toml", /^(version\s*=\s*")[^"]+(")/m),
  // The app's lock file also has the engine crates the app uses.
  edited(
    "app/src-tauri/Cargo.lock",
    ...[name, "dznav", "dznav-sys"].map((crate) => new RegExp(`^(name = "${crate}"\\r?\\nversion = ")[^"]+(")`, "m")),
  ),
  // CMake versions are numbers only, so a beta's is the version it leads up to.
  editedTo("CMakeLists.txt", version.split("-")[0], /^(project\(dznav VERSION )[\d.]+( )/m),
  // The first version in the engine's Cargo.toml is the workspace's, which its crates share.
  edited("engine/Cargo.toml", /^(version\s*=\s*")[^"]+(")/m),
  edited(
    "engine/Cargo.lock",
    ...["dznav", "dznav-cli", "dznav-sys"].map((crate) => new RegExp(`^(name = "${crate}"\\r?\\nversion = ")[^"]+(")`, "m")),
  ),
];

const changelog = readFileSync(path("CHANGELOG.md"), "utf8");
const releases = parseChangelog(changelog);
if (!releases.some((release) => release.version === version)) {
  const at = changelog.search(/^## /m);
  const section = `## ${version} - ${today()}\n\n`;
  const text =
    at === -1 ? `${changelog.trimEnd()}\n\n${section}` : changelog.slice(0, at) + section + changelog.slice(at);
  edits.push(["CHANGELOG.md", text]);
}
for (const [file, text] of edits) writeFileSync(path(file), text);

console.log(`\nVersion ${current} -> ${version}.\n`);
console.log("Write its release notes in CHANGELOG.md, following docs/release-notes.md.");
console.log("Then commit, push and run pnpm release.\n");

function edited(file: string, ...patterns: RegExp[]): [string, string] {
  return editedTo(file, version, ...patterns);
}

function editedTo(file: string, value: string, ...patterns: RegExp[]): [string, string] {
  let text = readFileSync(path(file), "utf8");
  for (const pattern of patterns) {
    if (!pattern.test(text)) fail(`Couldn't find the version in ${file}.`);
    text = text.replace(pattern, `$1${value}$2`);
  }
  return [file, text];
}

function today() {
  const now = new Date();
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
}

function fail(problem: string): never {
  console.error(`\n${problem}\n`);
  process.exit(1);
}
