// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Error, Result},
    byteorder::{LittleEndian, ReadBytesExt},
    serde::{Deserialize, Serialize},
    std::convert::TryFrom,
    std::io::{Cursor, Read},
    thiserror::Error,
};

/// ZBIs must start with a container type that contains all of the sections in
/// the ZBI. It is largely there to verify the binary blob is actually a ZBI and
/// to inform the reader how far to read into the blob.
const ZBI_TYPE_CONTAINER: u32 = 0x544f4f42;

/// LSW of sha256("bootdata")
const ZBI_CONTAINER_MAGIC: u32 = 0x868cf7e6;

/// LSW of sha256("bootitem")
const ZBI_ITEM_MAGIC: u32 = 0xb5781729;

/// Defines all of the known ZBI section types. These are used to partition
/// the Zircon boot image into sections.
#[repr(u32)]
pub enum ZbiType {
    Discard = 0x50494b53,
    StorageRamdisk = 0x4b534452,
    StorageBootfs = 0x42534642,
    StorageBootfsFactory = 0x46534642,
    Cmdline = 0x4c444d43,
    Crashlog = 0x4d4f4f42,
    Nvram = 0x4c4c564e,
    NvramDeprecated = 0x4c4c5643,
    PlatformId = 0x44494C50,
    DriverBoardInfo = 0x4953426D,
    CpuConfig = 0x43555043,
    CpuTopology = 0x544F504F,
    MemoryConfig = 0x434D454D,
    KernelDriver = 0x5652444B,
    AcpiRsdp = 0x50445352,
    Smbios = 0x49424d53,
    EfiMemoryMap = 0x4d494645,
    EfiSystemTable = 0x53494645,
    E820MemoryTable = 0x30323845,
    FrameBuffer = 0x42465753,
    ImageArgs = 0x47524149,
    BootVersion = 0x53525642,
    DriverMacAddress = 0x43414D6D,
    DriverPartitionMap = 0x5452506D,
    DriverBoardPrivate = 0x524F426D,
    RebootReason = 0x42525748,
    SerialNumber = 0x4e4c5253,
    BootloaderFile = 0x4C465442,
    Unknown = 0x0,
}

/// ZbiSection holder that contains the type and an uncompressed buffer
/// containing the data.
#[allow(dead_code)]
pub struct ZbiSection {
    section_type: ZbiType,
    buffer: Vec<u8>,
}

/// Rust clone of zircon/boot/image.h
#[allow(dead_code)]
#[derive(Serialize, Deserialize)]
struct ZbiHeader {
    zbi_type: u32,
    length: u32,
    extra: u32,
    flags: u32,
    reserved_0: u32,
    reserved_1: u32,
    magic: u32,
    crc32: u32,
}

impl ZbiHeader {
    pub fn parse(cursor: &mut Cursor<Vec<u8>>) -> Result<Self> {
        Ok(Self {
            zbi_type: cursor.read_u32::<LittleEndian>()?,
            length: cursor.read_u32::<LittleEndian>()?,
            extra: cursor.read_u32::<LittleEndian>()?,
            flags: cursor.read_u32::<LittleEndian>()?,
            reserved_0: cursor.read_u32::<LittleEndian>()?,
            reserved_1: cursor.read_u32::<LittleEndian>()?,
            magic: cursor.read_u32::<LittleEndian>()?,
            crc32: cursor.read_u32::<LittleEndian>()?,
        })
    }
}

#[derive(Error, Debug)]
pub enum ZbiError {
    #[error("Zbi container header type does not match expected value")]
    InvalidContainerHeader,
    #[error("Zbi container header magic value doesn't match expected value")]
    InvalidContainerMagic,
    #[error("Zbi item header magic value doesn't match expected value")]
    InvalidItemMagic,
}

/// Responsible for extracting the zbi from the package and reading the zbi
/// data from it.
pub struct ZbiReader {
    cursor: Cursor<Vec<u8>>,
}

impl ZbiReader {
    pub fn new(zbi_buffer: Vec<u8>) -> Self {
        Self { cursor: Cursor::new(zbi_buffer) }
    }

    pub fn parse(&mut self) -> Result<Vec<ZbiSection>> {
        // Parse the header and validate it is a ZBI.
        let container_header = ZbiHeader::parse(&mut self.cursor)?;
        if container_header.zbi_type != ZBI_TYPE_CONTAINER {
            return Err(Error::new(ZbiError::InvalidContainerHeader {}));
        }
        if container_header.magic != ZBI_CONTAINER_MAGIC {
            return Err(Error::new(ZbiError::InvalidContainerMagic {}));
        }
        let container_end = self.cursor.position() + (container_header.length as u64);

        let mut zbi_sections = vec![];

        if container_end == self.cursor.position() {
            return Ok(zbi_sections);
        }

        // Iterate until we cannot parse section headers anymore or reach
        // the end.
        while let Ok(section_header) = ZbiHeader::parse(&mut self.cursor) {
            if section_header.magic != ZBI_ITEM_MAGIC {
                return Err(Error::new(ZbiError::InvalidItemMagic {}));
            }

            let section_type = ZbiReader::section_type(section_header.zbi_type);
            let data_len = usize::try_from(section_header.length)?;
            let mut section_data = vec![0; data_len];
            self.cursor.read_exact(&mut section_data)?;

            zbi_sections.push(ZbiSection { section_type, buffer: section_data });

            // Exit if we have arrived at the end of the container length.
            if self.cursor.position() >= container_end {
                break;
            }
        }
        Ok(zbi_sections)
    }

    fn section_type(zbi_type: u32) -> ZbiType {
        match zbi_type {
            zbi_type if zbi_type == ZbiType::Discard as u32 => ZbiType::Discard,
            zbi_type if zbi_type == ZbiType::StorageRamdisk as u32 => ZbiType::StorageRamdisk,
            zbi_type if zbi_type == ZbiType::StorageBootfs as u32 => ZbiType::StorageBootfs,
            zbi_type if zbi_type == ZbiType::StorageBootfsFactory as u32 => {
                ZbiType::StorageBootfsFactory
            }
            zbi_type if zbi_type == ZbiType::Cmdline as u32 => ZbiType::Cmdline,
            zbi_type if zbi_type == ZbiType::Crashlog as u32 => ZbiType::Crashlog,
            zbi_type if zbi_type == ZbiType::Nvram as u32 => ZbiType::Nvram,
            zbi_type if zbi_type == ZbiType::NvramDeprecated as u32 => ZbiType::NvramDeprecated,
            zbi_type if zbi_type == ZbiType::PlatformId as u32 => ZbiType::PlatformId,
            zbi_type if zbi_type == ZbiType::DriverBoardInfo as u32 => ZbiType::DriverBoardInfo,
            zbi_type if zbi_type == ZbiType::CpuConfig as u32 => ZbiType::CpuConfig,
            zbi_type if zbi_type == ZbiType::CpuTopology as u32 => ZbiType::CpuTopology,
            zbi_type if zbi_type == ZbiType::MemoryConfig as u32 => ZbiType::MemoryConfig,
            zbi_type if zbi_type == ZbiType::KernelDriver as u32 => ZbiType::KernelDriver,
            zbi_type if zbi_type == ZbiType::AcpiRsdp as u32 => ZbiType::AcpiRsdp,
            zbi_type if zbi_type == ZbiType::Smbios as u32 => ZbiType::Smbios,
            zbi_type if zbi_type == ZbiType::EfiMemoryMap as u32 => ZbiType::EfiMemoryMap,
            zbi_type if zbi_type == ZbiType::EfiSystemTable as u32 => ZbiType::EfiSystemTable,
            zbi_type if zbi_type == ZbiType::E820MemoryTable as u32 => ZbiType::E820MemoryTable,
            zbi_type if zbi_type == ZbiType::FrameBuffer as u32 => ZbiType::FrameBuffer,
            zbi_type if zbi_type == ZbiType::ImageArgs as u32 => ZbiType::ImageArgs,
            zbi_type if zbi_type == ZbiType::BootVersion as u32 => ZbiType::BootVersion,
            zbi_type if zbi_type == ZbiType::DriverMacAddress as u32 => ZbiType::DriverMacAddress,
            zbi_type if zbi_type == ZbiType::DriverPartitionMap as u32 => {
                ZbiType::DriverPartitionMap
            }
            zbi_type if zbi_type == ZbiType::DriverBoardPrivate as u32 => {
                ZbiType::DriverBoardPrivate
            }
            zbi_type if zbi_type == ZbiType::RebootReason as u32 => ZbiType::RebootReason,
            zbi_type if zbi_type == ZbiType::SerialNumber as u32 => ZbiType::SerialNumber,
            zbi_type if zbi_type == ZbiType::BootloaderFile as u32 => ZbiType::BootloaderFile,
            _ => ZbiType::Unknown,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_zbi_empty_container() {
        let container_header = ZbiHeader {
            zbi_type: ZBI_TYPE_CONTAINER,
            length: 0,
            extra: 0,
            flags: 0,
            reserved_0: 0,
            reserved_1: 0,
            magic: ZBI_CONTAINER_MAGIC,
            crc32: 0,
        };
        let zbi_bytes = bincode::serialize(&container_header).unwrap();
        let mut reader = ZbiReader::new(zbi_bytes);
        let sections = reader.parse().unwrap();
        assert_eq!(sections.len(), 0);
    }

    #[test]
    fn test_zbi_secitons() {
        let mut container_header = ZbiHeader {
            zbi_type: ZBI_TYPE_CONTAINER,
            length: 0,
            extra: 0,
            flags: 0,
            reserved_0: 0,
            reserved_1: 0,
            magic: ZBI_CONTAINER_MAGIC,
            crc32: 0,
        };
        let section_header = ZbiHeader {
            zbi_type: ZbiType::Discard as u32,
            length: 10,
            extra: 10,
            flags: 0,
            reserved_0: 0,
            reserved_1: 0,
            magic: ZBI_ITEM_MAGIC,
            crc32: 0,
        };
        let section_data: Vec<u8> = vec![0; 10];

        let mut section_bytes: Vec<u8> = bincode::serialize(&section_header).unwrap();
        section_bytes.extend(&section_data);
        container_header.length = u32::try_from(section_bytes.len()).unwrap();
        let mut zbi_bytes: Vec<u8> = bincode::serialize(&container_header).unwrap();
        zbi_bytes.extend(&section_bytes);

        let mut reader = ZbiReader::new(zbi_bytes);
        let sections = reader.parse().unwrap();
        assert_eq!(sections.len(), 1);
        assert_eq!(sections[0].buffer.len(), 10);
    }
}
