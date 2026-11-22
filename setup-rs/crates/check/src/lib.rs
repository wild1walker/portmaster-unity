//! `pmu port check`, the check `pmu port new --data` starts from, and `pmu route` (port.py's
//! check with route.py's, plugins.py's and streaming.py's parts of it): what a Unity game is (its
//! build, Unity version, scripting backend), what it needs on a handheld (input, text, video,
//! native plugins, services), the route that fits and the port config values to start from.
//! Everything is read from the game's files; nothing of the game runs, nothing is changed.
pub mod android;
pub mod asm;
pub mod check;
pub mod pyx;
pub mod route;
pub mod suggest;
pub mod survey;
pub mod wwise;

pub use check::{check, report_json, report_text};
