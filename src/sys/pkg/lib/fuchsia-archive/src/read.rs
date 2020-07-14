// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    ChunkType, DirectoryEntry, Index, IndexEntry, DIRECTORY_ENTRY_LEN, DIR_CHUNK, DIR_NAMES_CHUNK,
    INDEX_ENTRY_LEN, MAGIC_INDEX_VALUE,
};
use anyhow::{format_err, Error};
use bincode::deserialize_from;
use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom};
use std::str;

/// A struct to open and read FAR-formatted archive.
pub struct Reader<'a, T>
where
    T: Read + Seek,
{
    source: &'a mut T,
    directory_entries: BTreeMap<String, DirectoryEntry>,
}

impl<'a, T> Reader<'a, T>
where
    T: Read + Seek,
{
    /// Create a new Reader for the provided source.
    pub fn new(source: &'a mut T) -> Result<Reader<'a, T>, Error> {
        let index = Reader::<T>::read_index(source)?;

        let (dir_index, dir_name_index) =
            Reader::<T>::read_index_entries(source, index.length / INDEX_ENTRY_LEN, &index)?;

        let dir_index = dir_index.ok_or(format_err!("Invalid archive, missing directory index"))?;
        let dir_name_index =
            dir_name_index.ok_or(format_err!("Invalid archive, missing directory name index"))?;

        source.seek(SeekFrom::Start(dir_name_index.offset))?;
        let mut path_data = vec![0; dir_name_index.length as usize];
        source.read_exact(&mut path_data)?;

        source.seek(SeekFrom::Start(dir_index.offset))?;
        let dir_entry_count = dir_index.length / DIRECTORY_ENTRY_LEN;
        let mut directory_entries = BTreeMap::new();
        for _ in 0..dir_entry_count {
            let entry: DirectoryEntry = deserialize_from(&mut *source)?;
            let name_start = entry.name_offset as usize;
            let after_name_end = name_start + entry.name_length as usize;
            let file_name_str = str::from_utf8(&path_data[name_start..after_name_end])?;
            let file_name = String::from(file_name_str);
            directory_entries.insert(file_name, entry);
        }

        Ok(Reader { source, directory_entries })
    }

    /// Return a list of the items in the archive
    pub fn list(&self) -> impl Iterator<Item = &str> {
        self.directory_entries.keys().map(String::as_str)
    }

    fn read_index(source: &mut T) -> Result<Index, Error> {
        let decoded_index: Index = deserialize_from(&mut *source)?;
        if decoded_index.magic != MAGIC_INDEX_VALUE {
            Err(format_err!("Invalid archive, bad magic"))
        } else if decoded_index.length % INDEX_ENTRY_LEN != 0 {
            Err(format_err!("Invalid archive, bad index length"))
        } else {
            Ok(decoded_index)
        }
    }

    fn read_index_entries(
        source: &mut T,
        count: u64,
        index: &Index,
    ) -> Result<(Option<IndexEntry>, Option<IndexEntry>), Error> {
        let mut dir_index: Option<IndexEntry> = None;
        let mut dir_name_index: Option<IndexEntry> = None;
        let mut last_chunk_type: Option<ChunkType> = None;
        for _ in 0..count {
            let entry: IndexEntry = deserialize_from(&mut *source)?;

            match last_chunk_type {
                None => {}
                Some(chunk_type) => {
                    if chunk_type > entry.chunk_type {
                        return Err(format_err!("Invalid archive, invalid index entry order"));
                    }
                }
            }

            last_chunk_type = Some(entry.chunk_type);

            if entry.offset < index.length {
                return Err(format_err!("Invalid archive, short offset"));
            }

            match entry.chunk_type {
                DIR_NAMES_CHUNK => {
                    dir_name_index = Some(entry);
                }
                DIR_CHUNK => {
                    dir_index = Some(entry);
                }
                _ => {
                    return Err(format_err!("Invalid archive, invalid chunk type"));
                }
            }
        }
        Ok((dir_index, dir_name_index))
    }

    fn find_directory_entry(&self, archive_path: &str) -> Result<&DirectoryEntry, Error> {
        self.directory_entries
            .get(archive_path)
            .ok_or(format_err!("Path '{}' not found in archive", archive_path))
    }

    /// Create an EntryReader for an entry with the specified name.
    pub fn open(&mut self, archive_path: &str) -> Result<EntryReader<'_, T>, Error> {
        let directory_entry = self.find_directory_entry(archive_path)?;

        Ok(EntryReader {
            offset: directory_entry.data_offset,
            length: directory_entry.data_length,
            source: self.source,
        })
    }

    /// Read the entire contents of an entry with the specified name.
    pub fn read_file(&mut self, archive_path: &str) -> Result<Vec<u8>, Error> {
        let mut reader = self.open(archive_path)?;
        reader.read_at(0)
    }

    /// Get the size in bytes of an entry with the specified name.
    pub fn get_size(&mut self, archive_path: &str) -> Result<u64, Error> {
        let directory_entry = self.find_directory_entry(archive_path)?;
        Ok(directory_entry.data_length)
    }
}

/// A structure that allows reading from the offset of an item in a
/// FAR archive.
pub struct EntryReader<'a, T>
where
    T: Read + Seek,
{
    offset: u64,
    length: u64,
    source: &'a mut T,
}

impl<'a, T> EntryReader<'a, T>
where
    T: Read + Seek,
{
    pub fn read_at(&mut self, offset: u64) -> Result<Vec<u8>, Error> {
        if offset > self.length {
            return Err(format_err!("Offset exceeds length of entry"));
        }
        self.source.seek(SeekFrom::Start(self.offset + offset))?;
        let clamped_length = self.length - offset;

        let mut data = vec![0; clamped_length as usize];
        self.source.read_exact(&mut data)?;
        Ok(data)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tests::example_archive;
    use crate::INDEX_LEN;
    use bincode::serialize_into;
    use itertools::assert_equal;
    use std::io::{Cursor, Seek, SeekFrom};
    use std::str;

    fn empty_archive() -> Vec<u8> {
        vec![0xc8, 0xbf, 0xb, 0x48, 0xad, 0xab, 0xc5, 0x11, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0]
    }

    fn corrupt_magic(b: &mut Vec<u8>) {
        b[0] = 0;
    }

    fn corrupt_index_length(b: &mut Vec<u8>) {
        let v: u64 = 1;
        let mut cursor = Cursor::new(b);
        cursor.seek(SeekFrom::Start(8)).unwrap();
        serialize_into(&mut cursor, &v).unwrap();
    }

    fn corrupt_dir_index_type(b: &mut Vec<u8>) {
        let v: u8 = 255;
        let mut cursor = Cursor::new(b);
        cursor.seek(SeekFrom::Start(INDEX_LEN)).unwrap();
        serialize_into(&mut cursor, &v).unwrap();
    }

    #[test]
    fn test_reader() {
        let example = example_archive();
        let mut example_cursor = Cursor::new(&example);
        assert!(Reader::new(&mut example_cursor).is_ok());

        let corrupters = [corrupt_magic, corrupt_index_length, corrupt_dir_index_type];
        let mut index = 0;
        for corrupter in corrupters.iter() {
            let mut example = example_archive();
            corrupter(&mut example);
            let mut example_cursor = Cursor::new(&mut example);
            let reader = Reader::new(&mut example_cursor);
            assert!(reader.is_err(), "corrupter index = {}", index);
            index += 1;
        }
    }

    #[test]
    fn test_list_files() {
        let example = example_archive();
        let mut example_cursor = Cursor::new(&example);
        let reader = Reader::new(&mut example_cursor).unwrap();

        let files = reader.list().collect::<Vec<_>>();
        let want = ["a", "b", "dir/c"];
        assert_equal(want.iter(), &files);
    }

    #[test]
    fn test_reader_open() {
        let example = example_archive();
        let mut example_cursor = Cursor::new(&example);
        let mut reader = Reader::new(&mut example_cursor).unwrap();
        assert_eq!(3, reader.directory_entries.len());

        for one_name in ["frobulate", "dir/enhunts"].iter() {
            let entry_reader = reader.open(one_name);
            assert!(entry_reader.is_err(), "Expected error for archive path \"{}\"", one_name);
        }

        for one_name in ["a", "b", "dir/c"].iter() {
            let mut entry_reader = reader.open(one_name).unwrap();
            let expected_error = entry_reader.read_at(99);
            assert!(expected_error.is_err(), "Expected error for offset that exceeds length");
            let content = entry_reader.read_at(0).unwrap();
            let content_str = str::from_utf8(&content).unwrap();
            let expected = format!("{}\n", one_name);
            assert_eq!(content_str, &expected);
        }
    }

    #[test]
    fn test_reader_read_file() {
        let example = example_archive();
        let mut example_cursor = Cursor::new(&example);
        let mut reader = Reader::new(&mut example_cursor).unwrap();
        for one_name in ["a", "b", "dir/c"].iter() {
            let content = reader.read_file(one_name).unwrap();
            let content_str = str::from_utf8(&content).unwrap();
            let expected = format!("{}\n", one_name);
            assert_eq!(content_str, &expected);
        }
    }

    #[test]
    fn test_reader_get_size() {
        let example = example_archive();
        let mut example_cursor = Cursor::new(&example);
        let mut reader = Reader::new(&mut example_cursor).unwrap();
        for one_name in ["a", "b", "dir/c"].iter() {
            let returned_size = reader.get_size(one_name).unwrap();
            let expected_size = one_name.len() + 1;
            assert_eq!(returned_size, expected_size as u64);
        }
    }

    #[test]
    fn test_read_empty() {
        let empty = empty_archive();
        let mut empty_cursor = Cursor::new(&empty);
        let reader = Reader::new(&mut empty_cursor);
        assert!(reader.is_err(), "Expected error for empty archive");
    }
}
