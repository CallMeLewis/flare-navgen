use std::path::Path;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// A navmesh (.nm) that doesn't parse.
    #[error("{0}")]
    Format(String),
    /// Data-server or capture data that doesn't parse.
    #[error("{0}")]
    Wire(String),
    /// A network failure talking to a data server (or serving as one).
    #[error("{0}")]
    Net(String),
    /// A failure reported by the C++ core.
    #[error("{0}")]
    Native(String),
    #[error("{context}: {source}")]
    Io {
        context: String,
        #[source]
        source: std::io::Error,
    },
}

pub type Result<T, E = Error> = std::result::Result<T, E>;

pub(crate) fn format_err<T>(message: impl Into<String>) -> Result<T> {
    Err(Error::Format(message.into()))
}

pub(crate) fn wire_err<T>(message: impl Into<String>) -> Result<T> {
    Err(Error::Wire(message.into()))
}

pub(crate) fn io_err(action: &str, path: &Path) -> impl FnOnce(std::io::Error) -> Error {
    let context = format!("cannot {action} {}", path.display());
    move |source| Error::Io { context, source }
}
