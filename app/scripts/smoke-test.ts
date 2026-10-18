// Installs a built Flare Navgen and fails if it closes within its first seconds, so a build that compiles but crashes on
// start never reaches anyone. CI runs it on every build, after making the build.
//
//   node scripts/smoke-test.ts <installer>
//
// The installer (*-setup.exe) is run silently into a scratch folder, and the app it installed is started. It only proves
// the app stays open, not that every screen works.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

/** How long the app has to stay open. Long enough to load the interface. */
const SECONDS = 20;
/** The app's program in the install folder (the engine, dznav.exe, sits next to it). */
const PROGRAM = "flare-navgen.exe";

const [file] = process.argv.slice(2);
if (!file) {
  console.error("Usage: node scripts/smoke-test.ts <installer>");
  process.exit(1);
}

try {
  await staysOpen(install(resolve(file)));
} catch (error) {
  console.error(`\n${error instanceof Error ? error.message : error}`);
  process.exit(1);
}

/** Installs silently into a scratch folder and returns the app it installed. */
function install(installer: string): string {
  const dir = mkdtempSync(join(tmpdir(), "flare-navgen-smoke-"));
  // NSIS takes the folder as the last argument, unquoted, even with spaces in it.
  const result = spawnSync(installer, ["/S", `/D=${dir}`], { stdio: "inherit" });
  if (result.error) throw new Error(`The installer couldn't be started: ${result.error.message}`);
  if (result.status !== 0) throw new Error(`The installer failed with exit code ${result.status}.`);
  const program = join(dir, PROGRAM);
  if (!existsSync(program)) throw new Error(`The installer didn't put ${PROGRAM} in ${dir}.`);
  if (!existsSync(join(dir, "dznav.exe"))) throw new Error(`The installer didn't put the engine, dznav.exe, in ${dir}.`);
  console.log(`Installed ${PROGRAM} and dznav.exe into ${dir}`);
  return program;
}

/** Starts the app, fails if it exits within SECONDS, then closes it and everything it started. */
async function staysOpen(program: string): Promise<void> {
  // Without checking for updates on its own, so CI doesn't count as someone using the app.
  const env = { ...process.env, FLARE_NO_AUTO_UPDATE_CHECK: "1" };
  const child = spawn(program, [], { stdio: "inherit", env });
  const exited = new Promise<string>((done) => {
    child.on("error", (error) => done(`couldn't be started: ${error.message}`));
    child.on("exit", (code, signal) => done(`closed after starting (${signal ?? `exit code ${code}`})`));
  });
  const timer = new Promise<null>((done) => setTimeout(() => done(null), SECONDS * 1000));
  const failure = await Promise.race([exited, timer]);
  if (failure) throw new Error(`The app ${failure}.`);

  console.log(`The app stayed open for ${SECONDS} seconds.`);
  spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" });
  await exited;
}
