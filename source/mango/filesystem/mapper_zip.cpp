/*
    MANGO Multimedia Development Platform
    Copyright (C) 2012-2018 Twilight Finland 3D Oy Ltd. All rights reserved.
*/
#include <mango/core/pointer.hpp>
#include <mango/core/string.hpp>
#include <mango/core/exception.hpp>
#include <mango/core/compress.hpp>
#include <mango/filesystem/mapper.hpp>
#include <mango/filesystem/path.hpp>
#include "indexer.hpp"

#include "../../external/miniz/miniz.h"

#define ID "[mapper.zip] "

/*
https://courses.cs.ut.ee/MTAT.07.022/2015_fall/uploads/Main/dmitri-report-f15-16.pdf

1] PKWARE Inc. APPNOTE.TXT – .ZIP File Format Specification, version 6.3.4
https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT

[2] PKWARE Inc. .ZIP Application Note
https://www.pkware.com/support/zip-app-note/

[3] PKWARE Inc. APPNOTE.TXT, version 1.0
https://pkware.cachefly.net/webdocs/APPNOTE/APPNOTE-1.0.txt

4] WinZip. AES Encryption Information: Encryption Specification AE-1 and AE-2
http://www.winzip.com/aes_info.htm
*/

namespace
{
    using namespace mango;

    using mango::filesystem::Indexer;

    enum { DCKEYSIZE = 12 };

    enum Encryption : u8
    {
        ENCRYPTION_NONE = 0,
        ENCRYPTION_CLASSIC = 1,
        ENCRYPTION_AES128 = 2,
        ENCRYPTION_AES192 = 3,
        ENCRYPTION_AES256 = 4,
    };

    enum Compression : u8
    {
        COMPRESSION_NONE = 0,
        COMPRESSION_DEFLATE = 8,
        COMPRESSION_DEFLATE64 = 9,
        COMPRESSION_BZIP2 = 12,
        COMPRESSION_WAVPACK = 97,
        COMPRESSION_PPMD = 98,
        COMPRESSION_LZMA = 14,
        COMPRESSION_JPEG = 96,
        COMPRESSION_AES = 99,
        COMPRESSION_XZ = 95
    };

    u32 getSaltLength(Encryption encryption)
    {
        u32 length = 0;
        switch (encryption)
        {
            case ENCRYPTION_AES128:
                length = 8;
                break;
            case ENCRYPTION_AES192:
                length = 12;
                break;
            case ENCRYPTION_AES256:
                length = 16;
                break;
            default:
                length = 0;
                break;
        }
        return length;
    }

    struct LocalFileHeader
    {
        uint32  signature;         // 0x04034b50 ("PK..")
        uint16  versionNeeded;     // version needed to extract
        uint16  flags;             //
        uint16  compression;       // compression method
        uint16  lastModTime;       // last mod file time
        uint16  lastModDate;       // last mod file date
        uint32  crc;               //
        uint64  compressedSize;    //
        uint64  uncompressedSize;  //
        uint16  filenameLen;       // length of the filename field following this structure
        uint16  extraFieldLen;     // length of the extra field following the filename field

        LocalFileHeader(LittleEndianPointer p)
        {
            signature = p.read32();
            if (status())
			{
			    versionNeeded    = p.read16();
			    flags            = p.read16();
			    compression      = p.read16();
			    lastModTime      = p.read16();
			    lastModDate      = p.read16();
			    crc              = p.read32();
			    compressedSize   = p.read32(); // ZIP64: 0xffffffff
			    uncompressedSize = p.read32(); // ZIP64: 0xffffffff
			    filenameLen      = p.read16();
			    extraFieldLen    = p.read16();

                p += filenameLen;

                // read extra fields
                uint8* ext = p;
                uint8* end = p + extraFieldLen;
                for (; ext < end;)
                {
                    LittleEndianPointer e = ext;
                    uint16 magic = e.read16();
                    uint16 size = e.read16();
                    uint8* next = e + size;
                    switch (magic)
                    {
                        case 0x0001:
                        {
                            // ZIP64 extended field
                            if (uncompressedSize == 0xffffffff) uncompressedSize = e.read64();
                            if (compressedSize == 0xffffffff) compressedSize = e.read64();
                            break;
                        }

                        case 0x9901:
                        {
                            // AES header
                            break;
                        }
                    }
                    ext = next;
                }
            }
        }

        bool status() const
        {
            return signature == 0x04034b50;
        }
	};

	struct DirFileHeader
	{
		uint32	signature;         // 0x02014b50
		uint16	versionUsed;       //
		uint16	versionNeeded;     //
		uint16	flags;             //
		uint16	compression;       // compression method
		uint16	lastModTime;       //
		uint16	lastModDate;       //
		uint32	crc;               //
		uint64	compressedSize;    // ZIP64: 0xffffffff
		uint64	uncompressedSize;  // ZIP64: 0xffffffff
		uint16	filenameLen;       // length of the filename field following this structure
		uint16	extraFieldLen;     // length of the extra field following the filename field
		uint16	commentLen;        // length of the file comment field following the extra field
		uint16	diskStart;         // the number of the disk on which this file begins, ZIP64: 0xffff
		uint16	internal;          // internal file attributes
		uint32	external;          // external file attributes
		uint64	localOffset;       // relative offset of the local file header, ZIP64: 0xffffffff

        std::string filename;      // filename is stored after the header
        bool        is_folder;     // if the last character of filename is "/", it is a folder
        Encryption  encryption;

		bool read(LittleEndianPointer& p)
		{
			signature = p.read32();
            if (signature != 0x02014b50)
            {
                return false;
            }

            versionUsed      = p.read16();
            versionNeeded    = p.read16();
            flags            = p.read16();
            compression      = p.read16();
            lastModTime      = p.read16();
            lastModDate      = p.read16();
            crc              = p.read32();
            compressedSize   = p.read32();
            uncompressedSize = p.read32();
            filenameLen      = p.read16();
            extraFieldLen    = p.read16();
            commentLen       = p.read16();
            diskStart        = p.read16();
            internal         = p.read16();
            external         = p.read32();
            localOffset      = p.read32();

            // read filename
            uint8* us = p;
            const char* s = reinterpret_cast<const char*>(us);
            p += filenameLen;

            if (s[filenameLen - 1] == '/')
            {
                is_folder = true;
            }
            else
            {
                is_folder = false;
            }

            filename = std::string(s, filenameLen);
            encryption = flags & 1 ? ENCRYPTION_CLASSIC : ENCRYPTION_NONE;

            // read extra fields
            uint8* ext = p;
            uint8* end = p + extraFieldLen;
            for ( ; ext < end;)
            {
                LittleEndianPointer e = ext;
                uint16 magic = e.read16();
                uint16 size = e.read16();
                uint8* next = e + size;
                switch (magic)
                {
                    case 0x0001:
                    {
                        // ZIP64 extended field
                        if (uncompressedSize == 0xffffffff) uncompressedSize = e.read64();
                        if (compressedSize == 0xffffffff) compressedSize = e.read64();
                        if (localOffset == 0xffffffff) localOffset = e.read64();
                        if (diskStart == 0xffff) e += 4;
                        break;
                    }

                    case 0x9901:
                    {
                        // AES header
                        u16 version = e.read16();
                        u16 magic = e.read16(); // must be 'AE' (0x41, 0x45)
                        u8 mode = e.read8();
                        compression = e.read8(); // override compression algorithm

                        if (version < 1 || version > 2 || magic != 0x4541)
                            MANGO_EXCEPTION(ID"Incorrect AES header.");

                        // select encryption mode
                        if (mode == 1) encryption = ENCRYPTION_AES128;
                        else if (mode == 2) encryption = ENCRYPTION_AES192;
                        else if (mode == 3) encryption = ENCRYPTION_AES256;
                        else MANGO_EXCEPTION(ID"Incorrect AES encryption mode.");

                        break;
                    }
                }
                ext = next;
            }

            p += extraFieldLen;
            p += commentLen;

            return true;
		}
	};

	struct DirEndRecord
	{
		uint32	signature;         // 0x06054b50
		uint16	thisDisk;          // number of this disk
		uint16	dirStartDisk;      // number of the disk containing the start of the central directory
		uint64	numEntriesOnDisk;  // # of entries in the central directory on this disk
		uint64	numEntriesTotal;   // total # of entries in the central directory
		uint64	dirSize;           // size of the central directory
		uint64	dirStartOffset;    // offset of the start of central directory on the disk
		uint16	commentLen;        // zip file comment length

		DirEndRecord(Memory memory)
		{
            std::memset(this, 0, sizeof(DirEndRecord));

			// find central directory end record signature
			// by scanning backwards from the end of the file
            uint8* start = memory.address;
            uint8* end = memory.address + memory.size;

            end -= 22; // header size is 22 bytes

            for (; end >= start; --end)
			{
                LittleEndianPointer p = end;

				signature = p.read32();
				if (status())
                {
                    // central directory detected
    				thisDisk         = p.read16();
	    			dirStartDisk     = p.read16();
		    		numEntriesOnDisk = p.read16();
			    	numEntriesTotal  = p.read16();
				    dirSize          = p.read32();
				    dirStartOffset   = p.read32();
				    commentLen       = p.read16();

                    if (thisDisk != 0 || dirStartDisk != 0 || numEntriesOnDisk != numEntriesTotal)
                    {
                        // multi-volume archives are not supported
                        signature = 0;
                    }

                    if (dirStartOffset == 0xffffffff)
                    {
                        p = end - 20;
                        uint32 magic = p.read32();
                        if (magic == 0x07064b50)
                        {
                            // ZIP64 detected
                            p += 4;
                            uint64 offset = p.read64();

                            p = start + offset;
                            magic = p.read32();
                            if (magic == 0x06064b50)
                            {
                                // ZIP64 End of Central Directory
                                p += 20;
		    		            numEntriesOnDisk = p.read64();
			    	            numEntriesTotal  = p.read64();
                                dirSize          = p.read64();
				                dirStartOffset   = p.read64();
                            }
                        }
                    }

                    break;
                }
			}
		}

        ~DirEndRecord()
        {
        }

        bool status() const
        {
            return signature == 0x06054b50;
        }
	};

    // --------------------------------------------------------------------
    // zip functions
    // --------------------------------------------------------------------

    inline uint32 zip_crc32(uint32 crc, uint8 v)
    {
        static const uint32 crc_table[] =
        {
            0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
            0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
        };

        uint32 x = crc;
        x = (x >> 4) ^ crc_table[(x & 0xf) ^ (v & 0xf)];
        x = (x >> 4) ^ crc_table[(x & 0xf) ^ (v >> 4)];
        return x;
	}

	inline uint8 zip_decrypt_value(uint32* keys)
	{
		uint32 temp = (keys[2] & 0xffff) | 2;
		return u8(((temp * (temp ^ 1)) >> 8) & 0xff);
	}

	inline void zip_update(uint32* keys, uint8 v)
	{
		keys[0] = zip_crc32(keys[0], v);
		keys[1] = 1 + (keys[1] + (keys[0] & 0xff)) * 134775813L;
		keys[2] = zip_crc32(keys[2], u8(keys[1] >> 24));
	}

	void zip_decrypt_buffer(uint8* out, const uint8* in, uint64 size, uint32* keys)
	{
		for (uint64 i = 0; i < size; ++i)
		{
			uint8 v = in[i] ^ zip_decrypt_value(keys);
			zip_update(keys, v);
			out[i] = v;
		}
	}

	void zip_init_keys(uint32* keys, const char* password)
	{
		keys[0] = 305419896L;
		keys[1] = 591751049L;
		keys[2] = 878082192L;

		for (; *password; ++password)
		{
			zip_update(keys, *password);
		}
	}

	bool zip_decrypt(uint8* out, const uint8* in, uint64 size, const uint8* dcheader, int version, uint32 crc, const std::string& password)
	{
		if (password.empty())
		{
			// missing password
		    return false;
		}

		// decryption keys
		uint32 keys[3];
		zip_init_keys(keys, password.c_str());

		// decrypt the 12 byte encryption header
		uint8 keyfile[DCKEYSIZE];
		zip_decrypt_buffer(keyfile, dcheader, DCKEYSIZE, keys);

		// check that password is correct one
		if (version < 20)
		{
			uint16* p = reinterpret_cast<uint16*>(&keyfile[10]);
			if (*p != (crc >> 16))
			{
				// incorrect password
				return false;
			}
		}
		else if (version < 30)
		{
			if (keyfile[11] != (crc >> 24))
			{
				// incorrect password
				return false;
			}
		}
		else
		{
		    // NOTE: the CRC/password check should be same for version > 3.0
		    //       but some compression programs don't create compatible
		    //       dcheader so the check would fail above.

#if 0 // TODO: the condition is reversed here.. need more testing (check specs!)
			if (keyfile[11] == (crc >> 24))
			{
				// incorrect password
				return false;
			}
#endif
		}

		// read compressed data & decrypt
		zip_decrypt_buffer(out, in, size, keys);

		return true;
	}

	uint64 zip_decompress(uint8* compressed, uint8* uncompressed, uint64 compressedLen, uint64 uncompressedLen)
	{
		z_stream zstream;
		std::memset(&zstream, 0, sizeof(zstream));

		zstream.next_in  = compressed;
		zstream.avail_in = uInt(compressedLen); // TODO: upgrade to support 64 bit files

        if (inflateInit2(&zstream, -MAX_WBITS) != Z_OK)
		{
            MANGO_EXCEPTION(ID"InflateInit failed.");
		}

    	// decompression
		zstream.next_out  = uncompressed;
	    zstream.avail_out = uInt(uncompressedLen); // TODO: upgrade to support 64 bit files

    	int zcode = inflate(&zstream, Z_FINISH);
		if (zcode != Z_STREAM_END)
        {
            const char* msg = ID"Internal error.";
            switch (zcode)
            {
                case Z_MEM_ERROR:
                    msg = ID"Memory error.";
                    break;

                case Z_BUF_ERROR:
                    msg = ID"Buffer error.";
                    break;

                case Z_DATA_ERROR:
                    msg = ID"Data error.";
                    break;
            }
            MANGO_EXCEPTION(msg);
        }

		if (inflateEnd(&zstream) != Z_OK)
	    {
            MANGO_EXCEPTION(ID"Inflate failed.");
		}

		return zstream.total_out;
    }

} // namespace

namespace mango
{

    // -----------------------------------------------------------------
    // VirtualMemoryZIP
    // -----------------------------------------------------------------

    class VirtualMemoryZIP : public mango::VirtualMemory
    {
    protected:
        uint8* m_delete_address;

    public:
        VirtualMemoryZIP(uint8* address, uint8* delete_address, size_t size)
            : m_delete_address(delete_address)
        {
            m_memory = Memory(address, size);
        }

        ~VirtualMemoryZIP()
        {
            delete [] m_delete_address;
        }
    };

    // -----------------------------------------------------------------
    // MapperZIP
    // -----------------------------------------------------------------

    class MapperZIP : public AbstractMapper
    {
    public:
        Memory m_parent_memory;
        std::string m_password;
        Indexer<DirFileHeader> m_folders;

        MapperZIP(Memory parent, const std::string& password)
            : m_parent_memory(parent)
            , m_password(password)
        {
            if (parent.address)
            {
                DirEndRecord record(parent);
                if (record.status())
                {
                    const int numFiles = int(record.numEntriesTotal);

                    // read file headers
                    LittleEndianPointer p = parent.address + record.dirStartOffset;

                    std::vector<DirFileHeader> headers;

                    for (int i = 0; i < numFiles; ++i)
                    {
                        DirFileHeader header;
                        if (header.read(p))
                        {
                            headers.push_back(header);
                        }
                    }

                    // find common prefix
                    size_t maxPrefix = headers[0].filename.length();

                    for (size_t i = 1; i < headers.size(); ++i)
                    {
                        size_t pos = 0;
                        for( ; pos < maxPrefix && 
                               pos < headers[i].filename.length() && 
                               headers[0].filename[pos] == headers[i].filename[pos]; pos++)
                        {
                        }

                        maxPrefix = pos;
                    }

                    std::string prefix = headers[0].filename.substr(0, maxPrefix);
                    prefix = getPath(prefix);

                    // store headers in a map
                    for (auto& header : headers)
                    {
                        header.filename = removePrefix(header.filename, prefix);

                        if (header.filename.length() > 0)
                        {
                            std::string folder = header.is_folder ?
                                getPath(header.filename.substr(0, header.filename.length() - 1)) :
                                getPath(header.filename);

                            m_folders.insert(folder, header.filename, header);
                        }
                    }
                }
            }
        }

        ~MapperZIP()
        {
        }

        VirtualMemory* mmap(const DirFileHeader& header, uint8* start, const std::string& password)
        {
            LittleEndianPointer p = start + header.localOffset;

            LocalFileHeader localHeader(p);
            if (!localHeader.status())
            {
                MANGO_EXCEPTION(ID"Invalid local header.");
            }

            uint64 offset = header.localOffset + 30 + localHeader.filenameLen + localHeader.extraFieldLen;

            uint8* address = start + offset;
            uint64 size = 0;

            uint8* buffer = nullptr; // remember allocated memory

            //printf("[ZIP] compression: %d, encryption: %d \n", header.compression, header.encryption);

            switch (header.encryption)
            {
                case ENCRYPTION_NONE:
                    break;

                case ENCRYPTION_CLASSIC:
                {
                    // decryption header
                    uint8* dcheader = address;
                    address += DCKEYSIZE;

                    // NOTE: decryption capability reduced on 32 bit platforms
                    const size_t compressed_size = size_t(header.compressedSize);
                    buffer = new uint8[compressed_size];

                    bool status = zip_decrypt(buffer, address, header.compressedSize, dcheader,
                                            header.versionUsed & 0xff, header.crc, password);
                    if (!status)
                    {
                        delete[] buffer;
                        MANGO_EXCEPTION(ID"Decryption failed (probably incorrect password).");
                    }

                    address = buffer;
                    break;
                }

                case ENCRYPTION_AES128:
                case ENCRYPTION_AES192:
                case ENCRYPTION_AES256:
                {
                    u32 salt_length = getSaltLength(header.encryption);
                    MANGO_UNREFERENCED_PARAMETER(salt_length);
                    MANGO_EXCEPTION(ID"AES encryption is not yet supported.");
#if 0                
                    u8* saltvalue = address;
                    address += salt_length;

                    u8* passverify = address;
                    address += AES_PWVERIFYSIZE;

                    address += HMAC_LENGTH;

                    size_t compressed_size = size_t(header.compressedSize);
                    compressed_size -= salt_length;
                    compressed_size -= AES_PWVERIFYSIZE;
                    compressed_size -= HMAC_LENGTH;

                    // TODO: password + salt --> key
                    // TODO: decrypt using the generated key
#endif                
                    break;
                }
            }

            switch (header.compression)
            {
                case COMPRESSION_NONE:
                    size = header.uncompressedSize;
                    break;

                case COMPRESSION_DEFLATE:
                {
                    const size_t uncompressed_size = size_t(header.uncompressedSize);
                    uint8* uncompressed_buffer = new uint8[uncompressed_size];

                    uint64 outsize = zip_decompress(address, uncompressed_buffer, header.compressedSize, header.uncompressedSize);

                    delete[] buffer;
                    buffer = uncompressed_buffer;

                    if (outsize != header.uncompressedSize)
                    {
                        // incorrect output size
                        delete[] buffer;
                        MANGO_EXCEPTION(ID"Incorrect decompressed size.");
                    }

                    // use decode_buffer as memory map
                    address = buffer;
                    size = header.uncompressedSize;
                    break;
                }

                case COMPRESSION_LZMA:
                {
                    const size_t uncompressed_size = size_t(header.uncompressedSize);
                    uint8* uncompressed_buffer = new uint8[uncompressed_size];

                    // parse LZMA compression header
                    p = address;
                    p += 2; // skip LZMA version
                    u16 lzma_propsize = p.read16();
                    if (lzma_propsize != 5)
                    {
                        delete[] buffer;
                        MANGO_EXCEPTION(ID"Incorrect LZMA header.");
                    }
                    address = p;
                    u64 compressed_size = header.compressedSize - 4;

                    lzma::decompress(Memory(uncompressed_buffer, size_t(header.uncompressedSize)), Memory(address, size_t(compressed_size)));

                    delete[] buffer;
                    buffer = uncompressed_buffer;

                    // use decode_buffer as memory map
                    address = buffer;
                    size = header.uncompressedSize;
                    break;
                }

                case COMPRESSION_PPMD:
                {
                    const std::size_t uncompressed_size = static_cast<std::size_t>(header.uncompressedSize);
                    uint8* uncompressed_buffer = new uint8[uncompressed_size];

                    ppmd8::decompress(Memory(uncompressed_buffer, size_t(header.uncompressedSize)), Memory(address, size_t(header.compressedSize)));

                    delete[] buffer;
                    buffer = uncompressed_buffer;

                    // use decode_buffer as memory map
                    address = buffer;
                    size = header.uncompressedSize;
                    break;
                }

                case COMPRESSION_BZIP2:
                {
                    const std::size_t uncompressed_size = static_cast<std::size_t>(header.uncompressedSize);
                    uint8* uncompressed_buffer = new uint8[uncompressed_size];

                    bzip2::decompress(Memory(uncompressed_buffer, size_t(header.uncompressedSize)), Memory(address, size_t(header.compressedSize)));

                    delete[] buffer;
                    buffer = uncompressed_buffer;

                    // use decode_buffer as memory map
                    address = buffer;
                    size = header.uncompressedSize;
                    break;
                }

                case COMPRESSION_DEFLATE64:
                case COMPRESSION_WAVPACK:
                case COMPRESSION_JPEG:
                case COMPRESSION_AES:
                case COMPRESSION_XZ:
                    MANGO_EXCEPTION(ID"Unsupported compression algorithm (%d).", header.compression);
                    break;
            }

            VirtualMemory* memory;
            if (buffer)
            {
                memory = new VirtualMemoryZIP(buffer, buffer, size_t(size));
            }
            else
            {
                memory = new VirtualMemoryZIP(address, nullptr, size_t(size));
            }

            return memory;
        }

        bool isFile(const std::string& filename) const override
        {
            const DirFileHeader* ptrFile = m_folders.file(filename);
            if (ptrFile)
            {
                return !ptrFile->is_folder;
            }
            return false;
        }

        void getIndex(FileIndex& index, const std::string& pathname) override
        {
            const Indexer<DirFileHeader>::Folder* ptrFolder = m_folders.folder(pathname);
            if (ptrFolder)
            {
                for (auto i : ptrFolder->files)
                {
                    const DirFileHeader& file = i.second;
                    std::string filename = i.first;

                    filename = filename.substr(pathname.length());

                    u32 flags = 0;
                    u64 size = file.uncompressedSize;

                    if (file.is_folder)
                    {
                        flags |= FileInfo::DIRECTORY;
                        size = 0;
                    }

                    if (file.compression > 0)
                    {
                        flags |= FileInfo::COMPRESSED;
                    }

                    if (file.encryption != ENCRYPTION_NONE)
                    {
                        flags |= FileInfo::ENCRYPTED;
                    }

                    index.emplace(filename, size, flags);
                }
            }

        }

        VirtualMemory* mmap(const std::string& filename) override
        {
            const DirFileHeader* ptrFile = m_folders.file(filename);
            if (!ptrFile)
            {
                MANGO_EXCEPTION(ID"File \"%s\" not found.", filename.c_str());
            }

            const DirFileHeader& file = *ptrFile;
            return mmap(file, m_parent_memory.address, m_password);
        }
    };

    // -----------------------------------------------------------------
    // functions
    // -----------------------------------------------------------------

    AbstractMapper* createMapperZIP(Memory parent, const std::string& password)
    {
        AbstractMapper* mapper = new MapperZIP(parent, password);
        return mapper;
    }

} // namespace mango
