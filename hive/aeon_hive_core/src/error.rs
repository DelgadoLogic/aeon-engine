// =============================================================================
// error.rs — AeonHive error types
// =============================================================================

use thiserror::Error;

#[derive(Error, Debug)]
pub enum HiveError {
    #[error("Identity error: {0}")]
    Identity(String),

    #[error("Node not started")]
    NotStarted,

    #[error("Connection failed: {0}")]
    Connection(String),

    #[error("Protocol error: {0}")]
    Protocol(String),

    #[error("Signature verification failed")]
    InvalidSignature,

    #[error("Peer not found: {0}")]
    PeerNotFound(String),

    #[error("Topic error: {0}")]
    Topic(String),

    #[error("IO error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Serialization error: {0}")]
    Serde(#[from] serde_json::Error),

    #[error("iroh error: {0}")]
    Iroh(#[from] anyhow::Error),
}

pub type HiveResult<T> = Result<T, HiveError>;
