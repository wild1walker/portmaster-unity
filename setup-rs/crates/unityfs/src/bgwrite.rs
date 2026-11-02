//! A file written on a thread of its own: what BufWriter does, with the writes made by another
//! thread while this one goes on (reading the next texture, encoding). On a handheld's card a
//! write() blocks once the kernel holds as much unwritten data as it allows, and the step's
//! thread then waits for the card instead of working. The bytes, their order and the seeks are
//! the caller's, so the file is the one a BufWriter writes. A write error comes back from a later
//! write, seek or flush (as from BufWriter's flush), and flush waits until everything is written.
use std::fs::File;
use std::io::{self, Seek, SeekFrom, Write};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::thread::JoinHandle;

enum Cmd {
    Data(Vec<u8>),
    SyncEvery(u64),
    Seek(SeekFrom, SyncSender<io::Result<u64>>),
    Flush(SyncSender<io::Result<()>>),
}

pub struct BgWriter {
    tx: Option<SyncSender<Cmd>>,
    thread: Option<JoinHandle<()>>,
    buf: Vec<u8>,
    chunk: usize,
    /// The position the caller sees: the bytes written since the last seek, from where it went.
    pos: u64,
}

/// Chunks of this size go to the thread, at most QUEUE of them waiting.
pub const CHUNK: usize = 1 << 20;
const QUEUE: usize = 4;

fn run(mut f: File, rx: Receiver<Cmd>) {
    let mut err: Option<io::Error> = None;
    let (mut every, mut since) = (0u64, 0u64);
    // (the first error, every time it is asked for: a later flush must not succeed)
    let take = |err: &mut Option<io::Error>| match err {
        None => Ok(()),
        Some(e) => Err(match e.raw_os_error() {
            Some(c) => io::Error::from_raw_os_error(c),
            None => io::Error::new(e.kind(), e.to_string()),
        }),
    };
    for c in rx {
        match c {
            Cmd::Data(d) => {
                if err.is_none() {
                    if let Err(e) = f.write_all(&d) {
                        err = Some(e);
                    }
                    since += d.len() as u64;
                    if every > 0 && since >= every {
                        // (the data on the card as it comes: the sync before the file is renamed
                        // into place then waits for the last part only)
                        since = 0;
                        if let Err(e) = f.sync_data() {
                            err = Some(e);
                        }
                    }
                }
            }
            Cmd::SyncEvery(n) => every = n,
            Cmd::Seek(to, reply) => {
                let r = take(&mut err).and_then(|_| f.seek(to));
                let _ = reply.send(r);
            }
            Cmd::Flush(reply) => {
                let r = take(&mut err).and_then(|_| f.flush());
                let _ = reply.send(r);
            }
        }
    }
}

fn gone() -> io::Error {
    io::Error::other("the writer thread stopped")
}

impl BgWriter {
    /// Writes to `f` from its current position (a file just made: 0).
    pub fn new(f: File) -> io::Result<BgWriter> {
        let (tx, rx) = sync_channel(QUEUE);
        let thread = std::thread::Builder::new()
            .name("pmu-writer".into())
            .spawn(move || run(f, rx))?;
        Ok(BgWriter {
            tx: Some(tx),
            thread: Some(thread),
            buf: Vec::with_capacity(CHUNK),
            chunk: CHUNK,
            pos: 0,
        })
    }

    /// A file made (truncated) at `path`, written on a thread.
    pub fn create(path: impl AsRef<std::path::Path>) -> io::Result<BgWriter> {
        BgWriter::new(File::create(path)?)
    }

    fn send(&mut self, c: Cmd) -> io::Result<()> {
        self.tx
            .as_ref()
            .ok_or_else(gone)?
            .send(c)
            .map_err(|_| gone())
    }

    fn push(&mut self) -> io::Result<()> {
        if self.buf.is_empty() {
            return Ok(());
        }
        let d = std::mem::replace(&mut self.buf, Vec::with_capacity(self.chunk));
        self.send(Cmd::Data(d))
    }

    /// Has the thread sync the file's data each time `n` more bytes are written (0: never).
    pub fn sync_every(&mut self, n: u64) -> io::Result<()> {
        self.send(Cmd::SyncEvery(n))
    }

    /// Everything written, and the thread ended: the first write error, if any.
    pub fn finish(mut self) -> io::Result<()> {
        self.flush()?;
        self.tx = None;
        if let Some(t) = self.thread.take() {
            t.join().map_err(|_| gone())?;
        }
        Ok(())
    }
}

impl Write for BgWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.buf.extend_from_slice(data);
        self.pos += data.len() as u64;
        if self.buf.len() >= self.chunk {
            self.push()?;
        }
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        self.push()?;
        let (rtx, rrx) = sync_channel(1);
        self.send(Cmd::Flush(rtx))?;
        rrx.recv().map_err(|_| gone())?
    }
}

impl Seek for BgWriter {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        if let SeekFrom::Current(0) = to {
            return Ok(self.pos);
        }
        self.push()?;
        // (relative seeks from the position the caller sees, which the file has once the
        // chunks before are written)
        let (rtx, rrx) = sync_channel(1);
        self.send(Cmd::Seek(to, rtx))?;
        self.pos = rrx.recv().map_err(|_| gone())??;
        Ok(self.pos)
    }
}

impl Drop for BgWriter {
    fn drop(&mut self) {
        // (what is left is written; errors are seen by a flush or finish before the drop)
        let _ = self.push();
        self.tx = None;
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    #[test]
    fn writes_and_seeks_as_a_file_does() {
        let dir = std::env::temp_dir().join(format!("unityfs-bgwrite-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("a.bin");
        let mut w = BgWriter::create(&p).unwrap();
        w.chunk = 7;
        let mut want = Vec::new();
        for i in 0..1000u32 {
            let b = i.to_le_bytes();
            w.write_all(&b).unwrap();
            want.extend_from_slice(&b);
        }
        assert_eq!(w.stream_position().unwrap(), 4000);
        w.seek(SeekFrom::Start(8)).unwrap();
        w.write_all(b"XYZ").unwrap();
        want[8..11].copy_from_slice(b"XYZ");
        assert_eq!(w.seek(SeekFrom::End(0)).unwrap(), 4000);
        w.write_all(b"end").unwrap();
        want.extend_from_slice(b"end");
        w.finish().unwrap();
        let mut got = Vec::new();
        File::open(&p).unwrap().read_to_end(&mut got).unwrap();
        assert_eq!(got, want);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_write_error_comes_back_from_flush() {
        // (/dev/full: every write fails with ENOSPC, as a full card's)
        let Ok(f) = File::options().write(true).open("/dev/full") else {
            return;
        };
        let mut w = BgWriter::new(f).unwrap();
        w.write_all(&vec![1u8; 3 * CHUNK]).unwrap();
        let e = w.flush().unwrap_err();
        assert_eq!(e.raw_os_error(), Some(28));
        assert_eq!(w.flush().unwrap_err().raw_os_error(), Some(28));
    }
}
