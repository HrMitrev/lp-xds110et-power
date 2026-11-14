//! Dedicated USB reader thread that drains the sample endpoint into a channel.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, SendTimeoutError, Sender, bounded};
use rusb::{Context, DeviceHandle};

use crate::config::{CMD_TIMEOUT_MS, DATA_IN_EP};

const READ_TIMEOUT: Duration = Duration::from_millis(200);
const CHANNEL_DEPTH: usize = 128;

pub type Chunk = Result<Vec<u8>, rusb::Error>;

/// Reads bulk data on its own thread so the USB endpoint FIFO never overruns while
/// the consumer is busy. The thread is stopped and joined when this value is dropped,
/// including on early returns.
pub struct ReaderThread {
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
    pub chunks: Receiver<Chunk>,
}

impl ReaderThread {
    pub fn spawn(dev: Arc<DeviceHandle<Context>>) -> Self {
        let (tx, chunks) = bounded(CHANNEL_DEPTH);
        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = Arc::clone(&stop);
        let handle = thread::spawn(move || read_loop(&dev, &tx, &thread_stop));
        Self {
            stop,
            handle: Some(handle),
            chunks,
        }
    }
}

impl Drop for ReaderThread {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

fn read_loop(dev: &DeviceHandle<Context>, tx: &Sender<Chunk>, stop: &AtomicBool) {
    let mut buf = vec![0u8; 0x4000];
    let mut last_data = Instant::now();
    let give_up_after = Duration::from_millis(CMD_TIMEOUT_MS);

    while !stop.load(Ordering::Relaxed) {
        let err = match dev.read_bulk(DATA_IN_EP, &mut buf, READ_TIMEOUT) {
            Ok(n) => {
                last_data = Instant::now();
                if n > 0 && !send_unless_stopped(tx, Ok(buf[..n].to_vec()), stop) {
                    return;
                }
                continue;
            }
            Err(e) => e,
        };

        match err {
            rusb::Error::Timeout => {}
            rusb::Error::Pipe => {
                // Endpoint stall/halt: clear halt and attempt to resume
                let _ = dev.clear_halt(DATA_IN_EP);
            }
            _ => thread::sleep(Duration::from_millis(50)),
        }

        if last_data.elapsed() >= give_up_after {
            send_unless_stopped(tx, Err(err), stop);
            return;
        }
    }
}

/// Blocking send that gives up when the consumer is gone or a stop is requested.
/// A plain `send` on the full channel would block forever once the consumer stops
/// receiving, which would deadlock the join in `Drop`.
fn send_unless_stopped(tx: &Sender<Chunk>, mut item: Chunk, stop: &AtomicBool) -> bool {
    loop {
        match tx.send_timeout(item, Duration::from_millis(50)) {
            Ok(()) => return true,
            Err(SendTimeoutError::Disconnected(_)) => return false,
            Err(SendTimeoutError::Timeout(returned)) => {
                if stop.load(Ordering::Relaxed) {
                    return false;
                }
                item = returned;
            }
        }
    }
}
