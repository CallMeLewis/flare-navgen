mod engine;

use tauri::{AppHandle, Manager};

/// The app's cache folder (created if needed), where tile previews are rendered.
#[tauri::command]
fn cache_dir(app: AppHandle) -> Result<String, String> {
    let dir = app.path().app_cache_dir().map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.to_string_lossy().into_owned())
}

/// Whether a file is there, so the window can ask before a run replaces it.
#[tauri::command]
fn file_exists(path: String) -> bool {
    std::path::Path::new(&path).is_file()
}

/// Opens the app's GitHub page in the browser.
#[tauri::command]
fn open_project_page(app: AppHandle) -> Result<(), String> {
    use tauri_plugin_opener::OpenerExt;
    app.opener()
        .open_url(env!("CARGO_PKG_REPOSITORY"), None::<&str>)
        .map_err(|e| format!("The browser couldn't be opened: {e}"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_flare_updater::updater())
        .plugin(
            // Must match `feedUrl` in app/scripts/release.ts.
            tauri_plugin_flare_updater::Builder::new("Flare-Navgen", "https://updates.darkzone.dev/flare-navgen/")
                // The installer closes the app straight away, so stop any engine run first.
                .on_before_exit(|app| app.state::<engine::Jobs>().kill_all())
                .build(),
        )
        .manage(engine::Jobs::default())
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(tauri_plugin_log::Builder::default().level(log::LevelFilter::Info).build())?;
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            cache_dir,
            file_exists,
            open_project_page,
            engine::engine_start,
            engine::engine_stop,
            engine::engine_kill
        ])
        .build(tauri::generate_context!())
        .expect("error while building tauri application")
        .run(|app, event| {
            // The window asks before closing during a run; by now the user has chosen to quit.
            if let tauri::RunEvent::Exit = event {
                app.state::<engine::Jobs>().kill_all();
            }
        });
}
