/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2012, 2010 Zheng Liu <lz@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::readers::{Reader, ReaderError},
    byteorder::LittleEndian,
    std::{collections::HashMap, mem::size_of, str, sync::Arc},
    zerocopy::{FromBytes, LayoutVerified, Unaligned, U16, U32, U64},
};

// Block Group 0 Padding
pub const FIRST_BG_PADDING: usize = 0x400;
// INode number of root directory '/'.
pub const ROOT_INODE_NUM: u32 = 2;
// EXT 2/3/4 magic number.
pub const SB_MAGIC: u16 = 0xEF53;
// Extent Header magic number.
pub const EH_MAGIC: u16 = 0xF30A;

type LEU16 = U16<LittleEndian>;
type LEU32 = U32<LittleEndian>;
type LEU64 = U64<LittleEndian>;

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct ExtentHeader {
    /// Magic number: 0xF30A
    pub eh_magic: LEU16,
    /// Number of valid entries.
    pub eh_ecount: LEU16,
    /// Entry capacity.
    pub eh_max: LEU16,
    /// Depth distance this node is from its leaves.
    /// `0` here means it is a leaf node.
    pub eh_depth: LEU16,
    /// Generation of extent tree.
    pub eh_gen: LEU32,
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(extent_header_size; ExtentHeader, [u8; 12]);

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct ExtentIndex {
    /// Indexes logical blocks.
    pub ei_blk: LEU32,
    /// Points to the physical block of the next level.
    pub ei_leaf_lo: LEU32,
    /// High 16 bits of physical block.
    pub ei_leaf_hi: LEU16,
    pub ei_unused: LEU16,
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(extent_index_size; ExtentIndex, [u8; 12]);

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct Extent {
    /// First logical block.
    pub e_blk: LEU32,
    /// Number of blocks.
    pub e_len: LEU16,
    /// High 16 bits of physical block.
    pub e_start_hi: LEU16,
    /// Low 16 bits of physical block.
    pub e_start_lo: LEU32,
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(extent_size; Extent, [u8; 12]);

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct DirEntry2 {
    /// INode number of entry
    pub e2d_ino: LEU32,
    /// Length of this record.
    pub e2d_reclen: LEU16,
    /// Length of string in `e2d_name`.
    pub e2d_namlen: u8,
    /// File type of this entry.
    pub e2d_type: u8,

    // TODO(vfcc): Actual size varies by e2d_reclen.
    // For now, we will read the max length and ignore the trailing bytes.
    /// Name of the entry.
    pub e2d_name: [u8; 255],
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(dir_entry_2_size; DirEntry2, [u8; 263]);

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct SuperBlock {
    /// INode count.
    pub e2fs_icount: LEU32,
    /// Block count.
    pub e2fs_bcount: LEU32,
    /// Reserved blocks count.
    pub e2fs_rbcount: LEU32,
    /// Free blocks count.
    pub e2fs_fbcount: LEU32,
    /// Free INodes count.
    pub e2fs_ficount: LEU32,
    /// First data block.
    pub e2fs_first_dblock: LEU32,
    /// Block Size = 2^(e2fs_log_bsize+10).
    pub e2fs_log_bsize: LEU32,
    /// Fragment size.
    pub e2fs_log_fsize: LEU32,
    /// Blocks per group.
    pub e2fs_bpg: LEU32,
    /// Fragments per group.
    pub e2fs_fpg: LEU32,
    /// INodes per group.
    pub e2fs_ipg: LEU32,
    /// Mount time.
    pub e2fs_mtime: LEU32,
    /// Write time.
    pub e2fs_wtime: LEU32,
    /// Mount count.
    pub e2fs_mnt_count: LEU16,
    /// Max mount count.
    pub e2fs_max_mnt_count: LEU16,
    /// Magic number: 0xEF53
    pub e2fs_magic: LEU16,
    /// Filesystem state.
    pub e2fs_state: LEU16,
    /// Behavior on errors.
    pub e2fs_beh: LEU16,
    /// Minor revision level.
    pub e2fs_minrev: LEU16,
    /// Time of last filesystem check.
    pub e2fs_lastfsck: LEU32,
    /// Max time between filesystem checks.
    pub e2fs_fsckintv: LEU32,
    /// Creator OS.
    pub e2fs_creator: LEU32,
    /// Revision level.
    pub e2fs_rev: LEU32,
    /// Default UID for reserved blocks.
    pub e2fs_ruid: LEU16,
    /// Default GID for reserved blocks.
    pub e2fs_rgid: LEU16,
    /// First non-reserved inode.
    pub e2fs_first_ino: LEU32,
    /// Size of INode structure.
    pub e2fs_inode_size: LEU16,
    /// Block group number of this super block.
    pub e2fs_block_group_nr: LEU16,
    /// Compatible feature set.
    pub e2fs_features_compat: LEU32,
    /// Incompatible feature set.
    pub e2fs_features_incompat: LEU32,
    /// RO-compatible feature set.
    pub e2fs_features_rocompat: LEU32,
    /// 128-bit uuid for volume.
    pub e2fs_uuid: [u8; 16],
    /// Volume name.
    pub e2fs_vname: [u8; 16],
    /// Name as mounted.
    pub e2fs_fsmnt: [u8; 64],
    /// Compression algorithm.
    pub e2fs_algo: LEU32,
    /// # of blocks for old prealloc.
    pub e2fs_prealloc: u8,
    /// # of blocks for old prealloc dirs.
    pub e2fs_dir_prealloc: u8,
    /// # of reserved gd blocks for resize.
    pub e2fs_reserved_ngdb: LEU16,
    /// UUID of journal super block.
    pub e3fs_journal_uuid: [u8; 16],
    /// INode number of journal file.
    pub e3fs_journal_inum: LEU32,
    /// Device number of journal file.
    pub e3fs_journal_dev: LEU32,
    /// Start of list of inodes to delete.
    pub e3fs_last_orphan: LEU32,
    /// HTREE hash seed.
    pub e3fs_hash_seed: [LEU32; 4],
    /// Default hash version to use.
    pub e3fs_def_hash_version: u8,
    /// Journal backup type.
    pub e3fs_jnl_backup_type: u8,
    /// size of group descriptor.
    pub e3fs_desc_size: LEU16,
    /// Default mount options.
    pub e3fs_default_mount_opts: LEU32,
    /// First metablock block group.
    pub e3fs_first_meta_bg: LEU32,
    /// When the filesystem was created.
    pub e3fs_mkfs_time: LEU32,
    /// Backup of the journal INode.
    pub e3fs_jnl_blks: [LEU32; 17],
    /// High bits of block count.
    pub e4fs_bcount_hi: LEU32,
    /// High bits of reserved blocks count.
    pub e4fs_rbcount_hi: LEU32,
    /// High bits of free blocks count.
    pub e4fs_fbcount_hi: LEU32,
    /// All inodes have some bytes.
    pub e4fs_min_extra_isize: LEU16,
    /// Inodes must reserve some bytes.
    pub e4fs_want_extra_isize: LEU16,
    /// Miscellaneous flags.
    pub e4fs_flags: LEU32,
    /// RAID stride.
    pub e4fs_raid_stride: LEU16,
    /// Seconds to wait in MMP checking.
    pub e4fs_mmpintv: LEU16,
    /// Block for multi-mount protection.
    pub e4fs_mmpblk: LEU64,
    /// Blocks on data disks (N * stride).
    pub e4fs_raid_stripe_wid: LEU32,
    /// FLEX_BG group size.
    pub e4fs_log_gpf: u8,
    /// Metadata checksum algorithm used.
    pub e4fs_chksum_type: u8,
    /// Versioning level for encryption.
    pub e4fs_encrypt: u8,
    pub e4fs_reserved_pad: u8,
    /// Number of lifetime kilobytes.
    pub e4fs_kbytes_written: LEU64,
    /// INode number of active snapshot.
    pub e4fs_snapinum: LEU32,
    /// Sequential ID of active snapshot.
    pub e4fs_snapid: LEU32,
    /// Reserved blocks for active snapshot.
    pub e4fs_snaprbcount: LEU64,
    /// INode number for on-disk snapshot.
    pub e4fs_snaplist: LEU32,
    /// Number of filesystem errors.
    pub e4fs_errcount: LEU32,
    /// First time an error happened.
    pub e4fs_first_errtime: LEU32,
    /// INode involved in first error.
    pub e4fs_first_errino: LEU32,
    /// Block involved of first error.
    pub e4fs_first_errblk: LEU64,
    /// Function where error happened.
    pub e4fs_first_errfunc: [u8; 32],
    /// Line number where error happened.
    pub e4fs_first_errline: LEU32,
    /// Most recent time of an error.
    pub e4fs_last_errtime: LEU32,
    /// INode involved in last error.
    pub e4fs_last_errino: LEU32,
    /// Line number where error happened.
    pub e4fs_last_errline: LEU32,
    /// Block involved of last error.
    pub e4fs_last_errblk: LEU64,
    /// Function where error happened.
    pub e4fs_last_errfunc: [u8; 32],
    /// Mount options.
    pub e4fs_mount_opts: [u8; 64],
    /// INode for tracking user quota.
    pub e4fs_usrquota_inum: LEU32,
    /// INode for tracking group quota.
    pub e4fs_grpquota_inum: LEU32,
    /// Overhead blocks/clusters.
    pub e4fs_overhead_clusters: LEU32,
    /// Groups with sparse_super2 SBs.
    pub e4fs_backup_bgs: [LEU32; 2],
    /// Encryption algorithms in use.
    pub e4fs_encrypt_algos: [u8; 4],
    /// Salt used for string2key.
    pub e4fs_encrypt_pw_salt: [u8; 16],
    /// Location of the lost+found inode.
    pub e4fs_lpf_ino: LEU32,
    /// INode for tracking project quota.
    pub e4fs_proj_quota_inum: LEU32,
    /// Checksum seed.
    pub e4fs_chksum_seed: LEU32,
    /// Padding to the end of the block.
    pub e4fs_reserved: [LEU32; 98],
    /// Super block checksum.
    pub e4fs_sbchksum: LEU32,
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(super_block_size; SuperBlock, [u8; 1024]);

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct BlockGroupDesc32 {
    /// Blocks bitmap block.
    pub ext2bgd_b_bitmap: LEU32,
    /// INodes bitmap block.
    pub ext2bgd_i_bitmap: LEU32,
    /// INodes table block.
    pub ext2bgd_i_tables: LEU32,
    /// # Free blocks.
    pub ext2bgd_nbfree: LEU16,
    /// # Free INodes.
    pub ext2bgd_nifree: LEU16,
    /// # Directories.
    pub ext2bgd_ndirs: LEU16,
    /// Block group flags.
    pub ext4bgd_flags: LEU16,
    /// Snapshot exclusion bitmap location.
    pub ext4bgd_x_bitmap: LEU32,
    /// Block bitmap checksum.
    pub ext4bgd_b_bmap_csum: LEU16,
    /// INode bitmap checksum.
    pub ext4bgd_i_bmap_csum: LEU16,
    /// Unused INode count.
    pub ext4bgd_i_unused: LEU16,
    /// Group descriptor checksum.
    pub ext4bgd_csum: LEU16,
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(block_group_desc_32_size; BlockGroupDesc32, [u8; 32]);

// TODO(vfcc): There are more fields in BlockGroupDesc if the filesystem is 64bit.
// Uncomment this when we add support.
// #[derive(FromBytes, Unaligned)]
// #[repr(C)]
// pub struct BlockGroupDesc64 {
//     pub base: BlockGroupDesc32,
//     pub ext4bgd_b_bitmap_hi: LEU32,
//     pub ext4bgd_i_bitmap_hi: LEU32,
//     pub ext4bgd_i_tables_hi: LEU32,
//     pub ext4bgd_nbfree_hi: LEU16,
//     pub ext4bgd_nifree_hi: LEU16,
//     pub ext4bgd_ndirs_hi: LEU16,
//     pub ext4bgd_i_unused_hi: LEU16,
//     pub ext4bgd_x_bitmap_hi: LEU32,
//     pub ext4bgd_b_bmap_csum_hi: LEU16,
//     pub ext4bgd_i_bmap_csum_hi: LEU16,
//     pub ext4bgd_reserved: LEU32,
// }
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
// assert_eq_size!(block_group_desc_64_size; BlockGroupDesc64, [u8; 64]);

#[derive(FromBytes, Unaligned)]
#[repr(C)]
pub struct INode {
    /// Access permission flags.
    pub e2di_mode: LEU16,
    /// Owner UID.
    pub e2di_uid: LEU16,
    /// Size (in bytes).
    pub e2di_size: LEU32,
    /// Access time.
    pub e2di_atime: LEU32,
    /// Change time.
    pub e2di_ctime: LEU32,
    /// Modification time.
    pub e2di_mtime: LEU32,
    /// Deletion time.
    pub e2di_dtime: LEU32,
    /// Owner GID.
    pub e2di_gid: LEU16,
    /// File link count.
    pub e2di_nlink: LEU16,
    /// Block count.
    pub e2di_nblock: LEU32,
    /// Status flags.
    pub e2di_flags: LEU32,
    /// INode version.
    pub e2di_version: [u8; 4],
    /// Extent tree.
    pub e2di_blocks: [u8; 60],
    /// Generation.
    pub e2di_gen: LEU32,
    /// EA block.
    pub e2di_facl: LEU32,
    /// High bits for file size.
    pub e2di_size_high: LEU32,
    /// Fragment address (obsolete).
    pub e2di_faddr: LEU32,
    /// High bits for block count.
    pub e2di_nblock_high: LEU16,
    /// High bits for EA block.
    pub e2di_facl_high: LEU16,
    /// High bits for Owner UID.
    pub e2di_uid_high: LEU16,
    /// High bits for Owner GID.
    pub e2di_gid_high: LEU16,
    /// High bits for INode checksum.
    pub e2di_chksum_lo: LEU16,
    pub e2di_lx_reserved: LEU16,
}
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
assert_eq_size!(inode_size; INode, [u8; 128]);

// TODO(vfcc): There are more fields in the INode table, but they depend on
// e2di_extra_isize.
// Uncomment this if, at a later date, we add support for these fields.
// pub struct INodeExtra {
//     pub base: INode,
//     pub e2di_extra_isize: LEU16,
//     pub e2di_chksum_hi: LEU16,
//     pub e2di_ctime_extra: LEU32,
//     pub e2di_mtime_extra: LEU32,
//     pub e2di_atime_extra: LEU32,
//     pub e2di_crtime: LEU32,
//     pub e2di_crtime_extra: LEU32,
//     pub e2di_version_hi: LEU32,
//     pub e2di_projid: LEU32,
// }
// Make sure our struct's size matches the Ext4 spec.
// https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout
// assert_eq_size!(INodeExtra, [u8; 160]);

#[derive(Fail, Debug, PartialEq)]
pub enum ParsingError {
    #[fail(display = "Unable to parse Super Block at 0x{:X}", _0)]
    ParseSuperBlock(usize),
    #[fail(display = "Invalid Super Block magic number {} should be 0xEF53", _0)]
    InvalidSuperBlockMagic(u16),
    #[fail(display = "Block number {} out of bounds.", _0)]
    BlockNumberOutOfBounds(u64),
    #[fail(display = "SuperBlock e2fs_log_bsize value invalid: {}", _0)]
    BlockSizeInvalid(u32),

    #[fail(display = "Unable to parse Block Group Description at 0x{:X}", _0)]
    ParseBlockGroupDesc(usize),
    #[fail(display = "Unable to parse INode {}", _0)]
    ParseINode(u32),

    // TODO(vfcc): A followup change will add the ability to include an address here.
    #[fail(display = "Unable to parse ExtentHeader from INode")]
    ParseExtentHeader,
    #[fail(display = "Invalid Extent Header magic number {} should be 0xF30A", _0)]
    InvalidExtentHeaderMagic(u16),
    #[fail(display = "Unable to parse Extent at 0x{:X}", _0)]
    ParseExtent(usize),
    #[fail(display = "Extent has more data {} than expected {}", _0, _1)]
    ExtentUnexpectedLength(usize, usize),

    #[fail(display = "Invalid Directory Entry at 0x{:X}", _0)]
    InvalidDirEntry2(usize),
    #[fail(display = "Directory Entry has invalid string in name field: {:?}", _0)]
    DirEntry2NonUtf8(Vec<u8>),
    #[fail(display = "Requested path contains invalid string")]
    InvalidInputPath,
    #[fail(display = "Non-existent path: {}", _0)]
    PathNotFound(String),
    #[fail(display = "Entry Type {} unknown", _0)]
    BadEntryType(u8),

    /// Message including what ext filesystem feature was found that we do not support
    #[fail(display = "{}", _0)]
    Incompatible(String),
    #[fail(display = "Bad file at {}", _0)]
    BadFile(String),
    #[fail(display = "Bad directory at {}", _0)]
    BadDirectory(String),

    #[fail(display = "Reader failed: {}", _0)]
    SourceError(ReaderError),
}

impl From<ReaderError> for ParsingError {
    fn from(err: ReaderError) -> ParsingError {
        ParsingError::SourceError(err)
    }
}

/// Directory Entry types.
#[derive(PartialEq)]
#[repr(u8)]
pub enum EntryType {
    Unknown = 0x0,
    RegularFile = 0x1,
    Directory = 0x2,
    CharacterDevice = 0x3,
    BlockDevice = 0x4,
    FIFO = 0x5,
    Socket = 0x6,
    SymLink = 0x7,
}

impl EntryType {
    pub fn from_u8(value: u8) -> Result<EntryType, ParsingError> {
        match value {
            0x0 => Ok(EntryType::Unknown),
            0x1 => Ok(EntryType::RegularFile),
            0x2 => Ok(EntryType::Directory),
            0x3 => Ok(EntryType::CharacterDevice),
            0x4 => Ok(EntryType::BlockDevice),
            0x5 => Ok(EntryType::FIFO),
            0x6 => Ok(EntryType::Socket),
            0x7 => Ok(EntryType::SymLink),
            _ => Err(ParsingError::BadEntryType(value)),
        }
    }
}

/// All functions to help parse data into respective structs.
pub trait ParseToStruct: FromBytes + Unaligned + Sized {
    fn parse_offset(
        reader: Arc<dyn Reader>,
        offset: usize,
        error_type: ParsingError,
    ) -> Result<Arc<Self>, ParsingError> {
        let data = Self::read_from_offset(reader, offset)?;
        Self::to_struct_arc(data, error_type)
    }

    fn read_from_offset(reader: Arc<dyn Reader>, offset: usize) -> Result<Box<[u8]>, ParsingError> {
        let mut data = vec![0u8; size_of::<Self>()];
        reader.read(offset, data.as_mut_slice())?;
        Ok(data.into_boxed_slice())
    }

    /// Transmutes from `Box<[u8]>` to `Arc<Self>`.
    ///
    /// `data` is consumed by this operation.
    ///
    /// `Self` is the ext4 struct that represents the given `data`.
    fn to_struct_arc(data: Box<[u8]>, error: ParsingError) -> Result<Arc<Self>, ParsingError> {
        Self::validate(&data, error)?;
        let data = Box::into_raw(data);
        unsafe { Ok(Arc::from_raw(data as *mut Self)) }
    }

    /// Casts the &[u8] data to &Self.
    ///
    /// `Self` is the ext4 struct that represents the given `data`.
    fn to_struct_ref(data: &[u8], error_type: ParsingError) -> Result<&Self, ParsingError> {
        LayoutVerified::<&[u8], Self>::new(data).map(|res| res.into_ref()).ok_or(error_type)
    }

    fn validate(data: &[u8], error_type: ParsingError) -> Result<(), ParsingError> {
        Self::to_struct_ref(data, error_type).map(|_| ())
    }
}

/// Apply to all EXT4 structs as seen above.
impl<T: FromBytes + Unaligned> ParseToStruct for T {}

impl SuperBlock {
    /// Parse the Super Block at its default location.
    pub fn parse(reader: Arc<dyn Reader>) -> Result<Arc<SuperBlock>, ParsingError> {
        // Super Block in Block Group 0 is at offset 1024.
        // Assuming there is no corruption, there is no need to read any other
        // copy of the Super Block.
        let data = SuperBlock::read_from_offset(reader, FIRST_BG_PADDING)?;
        let sb = SuperBlock::to_struct_arc(data, ParsingError::ParseSuperBlock(FIRST_BG_PADDING))?;
        if sb.e2fs_magic.get() == SB_MAGIC {
            Ok(sb)
        } else {
            Err(ParsingError::InvalidSuperBlockMagic(sb.e2fs_magic.get()))
        }
    }

    /// Reported block size.
    ///
    /// Realistically, would not exceed 64KiB. Let's arbitrarily cap at `usize` as this is
    /// usually the relevant datatype used during processing.
    ///
    /// We are currently enforcing `usize` to be `u64`, so the cap is unsigned 64bit.
    pub fn block_size(&self) -> Result<usize, ParsingError> {
        2usize
            .checked_pow(self.e2fs_log_bsize.get() + 10)
            .ok_or(ParsingError::BlockSizeInvalid(self.e2fs_log_bsize.get()))
    }
}

impl INode {
    /// INode contains the root of its Extent tree within `e2di_blocks`.
    /// Read `e2di_blocks` and return the root Extent Header.
    pub fn root_extent_header(&self) -> Result<&ExtentHeader, ParsingError> {
        let eh = ExtentHeader::to_struct_ref(
            // TODO(vfcc): Check the bounds here.
            &(self.e2di_blocks)[0..size_of::<ExtentHeader>()],
            ParsingError::ParseExtentHeader,
        )?;
        if eh.eh_magic.get() == EH_MAGIC {
            Ok(eh)
        } else {
            Err(ParsingError::InvalidExtentHeaderMagic(eh.eh_magic.get()))
        }
    }

    /// Size of the file/directory/entry represented by this INode.
    pub fn size(&self) -> usize {
        (self.e2di_size_high.get() as usize) << 32 | self.e2di_size.get() as usize
    }
}

impl DirEntry2 {
    /// Name of the file/directory/entry as a string.
    pub fn name(&self) -> Result<&str, ParsingError> {
        str::from_utf8(&self.e2d_name[0..self.e2d_namlen as usize]).map_err(|_| {
            ParsingError::DirEntry2NonUtf8(self.e2d_name[0..self.e2d_namlen as usize].to_vec())
        })
    }

    /// Generate a hash table of the given directory entries.
    ///
    /// Key: name of entry
    /// Value: DirEntry2 struct
    pub fn as_hash_map(
        entries: Vec<Arc<DirEntry2>>,
    ) -> Result<HashMap<String, Arc<DirEntry2>>, ParsingError> {
        let mut entry_map: HashMap<String, Arc<DirEntry2>> = HashMap::with_capacity(entries.len());

        for entry in entries {
            entry_map.insert(entry.name()?.to_string(), entry);
        }
        Ok(entry_map)
    }
}

#[cfg(test)]
mod test {
    use {
        super::{ParseToStruct, ParsingError, SuperBlock, FIRST_BG_PADDING, SB_MAGIC},
        crate::readers::VecReader,
        std::{fs, sync::Arc},
    };

    // NOTE: Impls for `INode` and `DirEntry2` depend on calculated data locations. Testing these
    // functions are being done in `parser.rs` where those locations are being calculated.

    // SuperBlock has a known data location and enables us to test `ParseToStruct` functions.

    /// Covers these functions:
    /// - ParseToStruct::{read_from_offset, to_struct_arc, to_struct_ref, validate}
    /// - SuperBlock::block_size
    #[test]
    fn parse_superblock() {
        let data = fs::read("/pkg/data/1file.img").expect("Unable to read file");
        let reader = Arc::new(VecReader::new(data));
        let sb = SuperBlock::parse(reader).expect("Parsed Super Block");
        assert_eq!(sb.e2fs_magic.get(), SB_MAGIC);
        // Block size of the 1file.img is 1KiB.
        assert_eq!(sb.block_size().unwrap(), FIRST_BG_PADDING);
    }

    /// Covers ParseToStruct::parse_offset.
    #[test]
    fn parse_to_struct_parse_offset() {
        let data = fs::read("/pkg/data/1file.img").expect("Unable to read file");
        let reader = Arc::new(VecReader::new(data));
        let sb = SuperBlock::parse_offset(
            reader,
            FIRST_BG_PADDING,
            ParsingError::ParseSuperBlock(FIRST_BG_PADDING),
        )
        .expect("Parsed Super Block");
        assert_eq!(sb.e2fs_magic.get(), SB_MAGIC);
    }
}
