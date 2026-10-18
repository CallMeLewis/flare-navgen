//! Runs the `dznav` engine (bundled as a sidecar) and streams its JSON-lines output to the UI.
//! The event format is documented in docs/cli-json.md.

use std::collections::HashMap;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering};

use serde::Serialize;
use tauri::{AppHandle, Manager, State, ipc::Channel};
use tauri_plugin_shell::ShellExt;
use tauri_plugin_shell::process::{CommandChild, CommandEvent};

#[derive(Default)]
pub struct Jobs {
    next_id: AtomicU32,
    children: Mutex<HashMap<u32, CommandChild>>,
}

impl Jobs {
    /// Kills every running engine process (used when the app exits).
    pub fn kill_all(&self) {
        let children: Vec<_> = self.children.lock().unwrap().drain().map(|(_, c)| c).collect();
        for child in children {
            let _ = child.kill();
        }
    }
}

#[derive(Clone, Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum EngineOutput {
    /// One line of stdout: a JSON event.
    Stdout { line: String },
    /// One line of stderr (diagnostics; the engine reports errors as JSON on stdout).
    Stderr { line: String },
    /// The process ended. `code` is None if it had none: on Windows even a killed process has one (1).
    Exit { code: Option<i32> },
}

/// Starts `dznav --json <args>` and returns a job id. Output arrives on `on_output`.
#[tauri::command]
pub fn engine_start(
    app: AppHandle,
    jobs: State<'_, Jobs>,
    args: Vec<String>,
    on_output: Channel<EngineOutput>,
) -> Result<u32, String> {
    let (mut rx, child) = app
        .shell()
        .sidecar("dznav")
        .map_err(|e| format!("engine not found: {e}"))?
        .arg("--json")
        .args(args)
        .spawn()
        .map_err(|e| format!("could not start the engine: {e}"))?;
    let id = jobs.next_id.fetch_add(1, Ordering::Relaxed) + 1;
    jobs.children.lock().unwrap().insert(id, child);

    tauri::async_runtime::spawn(async move {
        while let Some(event) = rx.recv().await {
            let out = match event {
                CommandEvent::Stdout(bytes) => EngineOutput::Stdout { line: line_of(&bytes) },
                CommandEvent::Stderr(bytes) => EngineOutput::Stderr { line: line_of(&bytes) },
                CommandEvent::Error(message) => EngineOutput::Stderr { line: message },
                CommandEvent::Terminated(status) => {
                    app.state::<Jobs>().children.lock().unwrap().remove(&id);
                    let _ = on_output.send(EngineOutput::Exit { code: status.code });
                    break;
                }
                _ => continue,
            };
            let _ = on_output.send(out);
        }
    });
    Ok(id)
}

/// Asks a running generation to stop: it finishes the tiles in progress and saves.
#[tauri::command]
pub fn engine_stop(jobs: State<'_, Jobs>, id: u32) -> Result<(), String> {
    let mut children = jobs.children.lock().unwrap();
    let child = children.get_mut(&id).ok_or("job is not running")?;
    child.write(b"stop\n").map_err(|e| e.to_string())
}

/// Ends a job at once, without saving.
#[tauri::command]
pub fn engine_kill(jobs: State<'_, Jobs>, id: u32) -> Result<(), String> {
    let child = jobs.children.lock().unwrap().remove(&id).ok_or("job is not running")?;
    child.kill().map_err(|e| e.to_string())
}

fn line_of(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).trim_end_matches(['\r', '\n']).to_string()
}
