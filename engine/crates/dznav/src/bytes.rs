//! Little-endian reading and writing of `#[repr(C)]` records. The formats are little-endian, and so are the targets
//! this builds for.

use bytemuck::{AnyBitPattern, NoUninit, Pod};

use crate::error::{Error, Result};

#[cfg(target_endian = "big")]
compile_error!("dznav reads and writes its formats in native byte order, which must be little-endian");

/// Reads records from a byte slice; running out of data is an error made by `err`.
pub(crate) struct Cursor<'a> {
    data: &'a [u8],
    pos: usize,
    err: fn(String) -> Error,
    truncated: &'static str,
}

impl<'a> Cursor<'a> {
    /// A cursor whose errors are made by `err`, "unexpected end of data" when the data runs out.
    pub fn new(data: &'a [u8], err: fn(String) -> Error) -> Self {
        Self { data, pos: 0, err, truncated: "unexpected end of data" }
    }

    /// Sets the message used when the data runs out.
    pub fn truncated_message(mut self, message: &'static str) -> Self {
        self.truncated = message;
        self
    }

    pub fn pos(&self) -> usize {
        self.pos
    }

    pub fn remaining(&self) -> usize {
        self.data.len() - self.pos
    }

    pub fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if n > self.remaining() {
            return Err((self.err)(self.truncated.to_string()));
        }
        let s = &self.data[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }

    pub fn read<T: AnyBitPattern>(&mut self) -> Result<T> {
        Ok(bytemuck::pod_read_unaligned(self.take(size_of::<T>())?))
    }

    pub fn read_vec<T: Pod>(&mut self, count: usize) -> Result<Vec<T>> {
        let n = count.checked_mul(size_of::<T>()).ok_or_else(|| (self.err)(self.truncated.to_string()))?;
        let src = self.take(n)?;
        let mut v = vec![T::zeroed(); count];
        bytemuck::cast_slice_mut(&mut v).copy_from_slice(src);
        Ok(v)
    }
}

pub(crate) fn put<T: NoUninit>(out: &mut Vec<u8>, v: &T) {
    out.extend_from_slice(bytemuck::bytes_of(v));
}

pub(crate) fn put_slice<T: NoUninit>(out: &mut Vec<u8>, v: &[T]) {
    out.extend_from_slice(bytemuck::cast_slice(v));
}
