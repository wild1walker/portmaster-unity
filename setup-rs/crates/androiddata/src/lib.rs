//! android-data and patch-obb pack (convert/pmunity android_data.py, patch_obb.py and what they
//! use on a setup's path: streaming.py, plugins.py, clrmeta.py, retarget.py, typetrees.py,
//! unity_version.py, video.py; winbcl.py), byte for byte: the APK and OBBs Unity's Android player reads, built from a
//! game's converted data, with the same state, report and exit statuses as the Python. The Unity
//! files go through unityfs, the archives through pkgzip's writer.
pub mod android_data;
pub mod args;
pub mod clock;
pub mod clrmeta;
pub mod ggm;
pub mod inline;
pub mod pack;
pub mod patch_obb;
pub mod paths;
pub mod plugins;
pub mod pyjson;
pub mod retarget;
pub mod sha1;
pub mod streaming;
pub mod typetrees;
pub mod uversion;
pub mod verify;
pub mod video;
pub mod winbcl;

pub use android_data::{main as android_data_main, Stop};
