// Publishes Flare Navgen's releases to GitHub and its update feed, with the release script every Flare app shares
// (@flare/release, in flare-common). Run from app/:
//
//   node scripts/release.ts release           (pnpm release) tag the commit and push the tag, once CI passed on it,
//                                             which starts the Release workflow
//   node scripts/release.ts publish           (the workflow) sign the installer, publish the GitHub release, then the
//                                             update feed's channel files
//   node scripts/release.ts feed <dir> [url]  sign a local build and write a feed to <dir>, for testing
//   node scripts/release.ts check             (CI) go through publishing as far as it can without the signing key
//   node scripts/release.ts keygen            create the update signing key (once) and print its public key
//
// The feed is https://updates.darkzone.dev/flare-navgen/, which must match the feed URL given to the updater plugin in
// src-tauri/src/lib.rs: latest.json for the stable channel and beta.json for the beta channel.
import { release } from "@flare/release";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const app = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const root = resolve(app, "..");
const target = join(app, "src-tauri", "target");

await release({
  name: "Flare Navgen",
  app,
  root,
  repo: "CallMeLewis/flare-navgen",
  feed: "flare-navgen",
  keyPath: process.env.FLARE_NAVGEN_UPDATE_SIGNING_KEY ?? join(homedir(), ".flare-navgen", "update-signing-key"),
  script: "node scripts/release.ts",
  secretEnvironment: "release",
  // Beside this repo, as the app links to it, and on the tag in .flare-common-version.
  common: { dir: resolve(root, "..", "flare-common"), versionFile: join(root, ".flare-common-version") },
  // DayZDiag only runs on Windows, so the app only ships for Windows.
  builds: [
    {
      platform: "windows-x86_64",
      paths: (product, version) => [join(target, "release", "bundle", "nsis", `${product}_${version}_x64-setup.exe`)],
      uploadName: (version) => `Flare-Navgen-Setup-${version}.exe`,
      buildWith: "pnpm dist (in app/)",
    },
  ],
});
