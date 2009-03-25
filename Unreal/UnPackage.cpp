#include "Core.h"

#include "UnCore.h"
#include "UnObject.h"
#include "UnPackage.h"

/*-----------------------------------------------------------------------------
	Lineage2 file reader
-----------------------------------------------------------------------------*/

#if LINEAGE2 || EXTEEL

#define LINEAGE_HEADER_SIZE		28

class FFileReaderLineage : public FArchive
{
public:
	FArchive	*Reader;
	int			ArPosOffset;
	byte		XorKey;

	FFileReaderLineage(FArchive *File, int Key)
	:	Reader(File)
	,	ArPosOffset(LINEAGE_HEADER_SIZE)
	,	XorKey(Key)
	{
		Seek(0);		// skip header
	}

	virtual void Serialize(void *data, int size)
	{
		Reader->Serialize(data, size);
		if (XorKey)
		{
			int i;
			byte *p;
			for (i = 0, p = (byte*)data; i < size; i++, p++)
				*p ^= XorKey;
		}
	}
	virtual void Seek(int Pos)
	{
		Reader->Seek(Pos + ArPosOffset);
	}
	virtual int  Tell() const
	{
		return Reader->Tell() - ArPosOffset;
	}
	virtual void SetStopper(int Pos)
	{
		Reader->SetStopper(Pos + ArPosOffset);
	}
	virtual int  GetStopper() const
	{
		return Reader->GetStopper() - ArPosOffset;
	}
};

#endif // LINEAGE2 || EXTEEL


/*-----------------------------------------------------------------------------
	UE3 compressed file reader
-----------------------------------------------------------------------------*/

#if UNREAL3

class FUE3ArchiveReader : public FArchive
{
public:
	FArchive				*Reader;
	// compression data
	int						CompressionFlags;
	const TArray<FCompressedChunk> *CompressedChunks;
	// own file positions, overriding FArchive's one (because parent class is
	// used for compressed data)
	int						Stopper;
	int						Position;
	// decompression buffer
	byte					*Buffer;
	int						BufferSize;
	int						BufferStart;
	int						BufferEnd;
	// chunk
	const FCompressedChunk	*CurrentChunk;
	FCompressedChunkHeader	ChunkHeader;
	int						ChunkDataPos;

	FUE3ArchiveReader(FArchive *File, int Flags, const TArray<FCompressedChunk> *Chunks)
	:	Reader(File)
	,	CompressionFlags(Flags)
	,	CompressedChunks(Chunks)
	,	Buffer(NULL)
	,	BufferSize(0)
	,	BufferStart(0)
	,	BufferEnd(0)
	,	CurrentChunk(NULL)
	{
		guard(FUE3ArchiveReader::FUE3ArchiveReader);
		ReverseBytes = File->ReverseBytes;
		assert(CompressionFlags);
		assert(Chunks->Num());
		unguard;
	}

	~FUE3ArchiveReader()
	{
		if (Buffer) delete Buffer;
		if (Reader) delete Reader;
	}

	virtual void Serialize(void *data, int size)
	{
		guard(FUE3ArchiveReader::Serialize);

		if (Stopper > 0 && Position + size > Stopper)
			appError("Serializing behind stopper");

		while (true)
		{
			// check for valid buffer
			if (Position >= BufferStart && Position < BufferEnd)
			{
				int ToCopy = BufferEnd - Position;						// available size
				if (ToCopy > size) ToCopy = size;						// shrink by required size
				memcpy(data, Buffer + Position - BufferStart, ToCopy);	// copy data
				// advance pointers/counters
				Position += ToCopy;
				size     -= ToCopy;
				data     = OffsetPointer(data, ToCopy);
				if (!size) return;										// copied enough
			}
			// here: data/size points outside of loaded Buffer
			PrepareBuffer(Position);
			assert(Position >= BufferStart && Position < BufferEnd);	// validate PrepareBuffer()
		}

		unguard;
	}

	void PrepareBuffer(int Pos)
	{
		guard(FUE3ArchiveReader::PrepareBuffer);
		// find compressed chunk
		const FCompressedChunk *Chunk = NULL;
		for (int ChunkIndex = 0; ChunkIndex < CompressedChunks->Num(); ChunkIndex++)
		{
			Chunk = &(*CompressedChunks)[ChunkIndex];
			if (Pos >= Chunk->UncompressedOffset && Pos < Chunk->UncompressedOffset + Chunk->UncompressedSize)
				break;
		}
		assert(Chunk); // should be found

		if (Chunk != CurrentChunk)
		{
			// serialize compressed chunk header
			Reader->Seek(Chunk->CompressedOffset);
			*Reader << ChunkHeader;
			ChunkDataPos = Reader->Tell();
			CurrentChunk = Chunk;
		}
		// find block in ChunkHeader.Blocks
		int ChunkPosition = Chunk->UncompressedOffset;
		int ChunkData     = ChunkDataPos;
		int UncompSize = 0, CompSize = 0;
		assert(ChunkPosition <= Pos);
		const FCompressedChunkBlock *Block = NULL;
		for (int BlockIndex = 0; BlockIndex < ChunkHeader.Blocks.Num(); BlockIndex++)
		{
			Block = &ChunkHeader.Blocks[BlockIndex];
			if (ChunkPosition + Block->UncompressedSize > Pos)
				break;
			ChunkPosition += Block->UncompressedSize;
			ChunkData     += Block->CompressedSize;
		}
		assert(Block);
		// read compressed data
		//?? optimize? can share compressed buffer and decompressed buffer between packages
		byte *CompressedBlock = new byte[Block->CompressedSize];
		Reader->Seek(ChunkData);
		Reader->Serialize(CompressedBlock, Block->CompressedSize);
		// prepare buffer for decompression
		if (Block->UncompressedSize > BufferSize)
		{
			if (Buffer) delete Buffer;
			Buffer = new byte[Block->UncompressedSize];
			BufferSize = Block->UncompressedSize;
		}
		// decompress data
		appDecompress(CompressedBlock, Block->CompressedSize, Buffer, Block->UncompressedSize, CompressionFlags);
		// setup BufferStart/BufferEnd
		BufferStart = ChunkPosition;
		BufferEnd   = ChunkPosition + Block->UncompressedSize;
		// cleanup
		delete CompressedBlock;
		unguard;
	}

	// position controller
	virtual void Seek(int Pos)
	{
		Position = Pos;
	}
	virtual int  Tell() const
	{
		return Position;
	}
	virtual void SetStopper(int Pos)
	{
		Stopper = Pos;
	}
	virtual int  GetStopper() const
	{
		return Stopper;
	}
};

#endif // UNREAL3

/*-----------------------------------------------------------------------------
	Package loading (creation) / unloading
-----------------------------------------------------------------------------*/

UnPackage::UnPackage(const char *filename, FArchive *Ar)
:	Loader(NULL)
{
	guard(UnPackage::UnPackage);

	// setup FArchive
	Loader = (Ar) ? Ar : new FFileReader(filename);
	IsLoading = true;

	appStrncpyz(Filename, filename, ARRAY_COUNT(Filename));

#if LINEAGE2 || EXTEEL
	int checkDword;
	*this << checkDword;
	if (checkDword == ('L' | ('i' << 16)))	// unicode string "Lineage2Ver111"
	{
		// this is a Lineage2 package
		Seek(LINEAGE_HEADER_SIZE);
		// here is a encrypted by 'xor' standard FPackageFileSummary
		// to get encryption key, can check 1st byte
		byte b;
		*this << b;
		// for Ver111 XorKey==0xAC for Lineage or ==0x42 for Exteel, for Ver121 computed from filename
		byte XorKey = b ^ (PACKAGE_FILE_TAG & 0xFF);
	#if LINEAGE2
		IsLineage2  = 1;
	#endif
	#if EXTEEL
		IsExteel    = 1;
	#endif
		// replace Loader
		Loader = new FFileReaderLineage(Loader, XorKey);
	}
	else
		Seek(0);	// seek back to header
#endif

	// read summary
	*this << Summary;
	Loader->ReverseBytes = ReverseBytes;	//!! should implement as virtual function
	ArVer         = Summary.FileVersion;
	ArLicenseeVer = Summary.LicenseeVersion;
	PKG_LOG(("Loading package: %s Ver: %d/%d Names: %d Exports: %d Imports: %d\n", Filename,
		Summary.FileVersion, Summary.LicenseeVersion,
		Summary.NameCount, Summary.ExportCount, Summary.ImportCount));

#if UNREAL3
	if (ArVer >= PACKAGE_V3 && Summary.CompressionFlags)
	{
		// replace Loader with special reader for compressed UE3 archives
		Loader = new FUE3ArchiveReader(Loader, Summary.CompressionFlags, &Summary.CompressedChunks);
	}
#endif

	// different game platforms autodetection
	//?? should change this, if will implement command line switch to force mode
	//?? code moved here, check code of other structs loaded below for ability to use Ar.IsGameName...
#if UT2
	IsUT2 = ((ArVer >= 117 && ArVer <= 120) && (ArLicenseeVer >= 0x19 && ArLicenseeVer <= 0x1C)) ||
			((ArVer >= 121 && ArVer <= 128) && ArLicenseeVer == 0x1D);
#endif
#if PARIAH
	IsPariah = (ArVer == 119 && ArLicenseeVer == 0x9127);
#endif
#if SPLINTER_CELL
	IsSplinterCell = (ArVer == 100 && (ArLicenseeVer >= 0x09 && ArLicenseeVer <= 0x11)) ||
					 (ArVer == 102 && (ArLicenseeVer >= 0x14 && ArLicenseeVer <= 0x1C));
#endif
#if TRIBES3
	IsTribes3 = ((ArVer == 129 || ArVer == 130) && (ArLicenseeVer >= 0x17 && ArLicenseeVer <= 0x1B)) ||
				((ArVer == 123) && (ArLicenseeVer >= 3    && ArLicenseeVer <= 0xF )) ||
				((ArVer == 126) && (ArLicenseeVer >= 0x12 && ArLicenseeVer <= 0x17));
#endif
#if LINEAGE2
	if (IsLineage2 && (ArLicenseeVer >= 1000))	// lineage LicenseeVer < 1000
		IsLineage2 = 0;
#endif
#if EXTEEL
	if (IsExteel && (ArLicenseeVer < 1000))		// exteel LicenseeVer >= 1000
		IsExteel = 0;
#endif

	// read name table
	guard(ReadNameTable);
	if (Summary.NameCount > 0)
	{
		Seek(Summary.NameOffset);
		NameTable = new char*[Summary.NameCount];
		for (int i = 0; i < Summary.NameCount; i++)
		{
			if (Summary.FileVersion < 64)
			{
				char buf[256];
				int len;
				for (len = 0; len < ARRAY_COUNT(buf); len++)
				{
					char c;
					*this << c;
					buf[len] = c;
					if (!c) break;
				}
				assert(len < ARRAY_COUNT(buf));
				NameTable[i] = strdup(buf);
				// skip object flags
				int tmp;
				*this << tmp;
			}
#if PARIAH
			else if (IsPariah && ((ArLicenseeVer & 0x3F) >= 0x1C))
			{
				// used word + char[] instead of FString
				word len;
				*this << len;
				NameTable[i] = new char[len+1];
				Serialize(NameTable[i], len+1);
				// skip object flags
				int tmp;
				*this << tmp;
			}
#endif
			else
			{
#if 0
				// FString, but less allocations ...
				int len;
				*this << AR_INDEX(len);
				char *s = NameTable[i] = new char[len];
				while (len-- > 0)
					*this << *s++;
#else
				// Lineage sometimes uses Unicode strings ...
				FString name;
				*this << name;
				NameTable[i] = new char[name.Num()];
				strcpy(NameTable[i], *name);
#endif
				// skip object flags
				int tmp;
				*this << tmp;
	#if UNREAL3
				if (ArVer >= PACKAGE_V3)
				{
					// object flags are 64-bit in UE3, skip additional 32 bits
					int unk;
					*this << unk;
				}
	#endif // UNREAL3
			}
//			PKG_LOG(("Name[%d]: \"%s\"\n", i, NameTable[i]));
		}
	}
	unguard;

	// load import table
	guard(ReadImportTable);
	if (Summary.ImportCount > 0)
	{
		Seek(Summary.ImportOffset);
		FObjectImport *Imp = ImportTable = new FObjectImport[Summary.ImportCount];
		for (int i = 0; i < Summary.ImportCount; i++, Imp++)
		{
			*this << *Imp;
//			PKG_LOG(("Import[%d]: %s'%s'\n", i, *Imp->ClassName, *Imp->ObjectName));
		}
	}
	unguard;

	// load exports table
	guard(ReadExportTable);
	if (Summary.ExportCount > 0)
	{
		Seek(Summary.ExportOffset);
		FObjectExport *Exp = ExportTable = new FObjectExport[Summary.ExportCount];
		for (int i = 0; i < Summary.ExportCount; i++, Exp++)
		{
			*this << *Exp;
//			PKG_LOG(("Export[%d]: %s'%s' offs=%08X size=%08X\n", i, GetObjectName(Exp->ClassIndex),
//				*Exp->ObjectName, Exp->SerialOffset, Exp->SerialSize));
		}
	}
	unguard;

#if UNREAL3
	if (Summary.FileVersion >= 415) // PACKAGE_V3
	{
		guard(ReadDependsTable);
		Seek(Summary.DependsOffset);
		FObjectDepends *Dep = DependsTable = new FObjectDepends[Summary.ExportCount];
		for (int i = 0; i < Summary.ExportCount; i++, Dep++)
		{
			*this << *Dep;
/*			if (Dep->Objects.Num())
			{
				const FObjectExport &Exp = ExportTable[i];
				printf("Depends for %s'%s' = %d\n", GetObjectName(Exp.ClassIndex),
					*Exp.ObjectName, Dep->Objects[i]);
			} */
		}
		unguard;
	}
#endif // UNREAL3

	// add self to package map
	PackageEntry &Info = PackageMap[PackageMap.Add()];
	char buf[256];
	const char *s = strrchr(filename, '/');
	if (!s) s = strrchr(filename, '\\');			// WARNING: not processing mixed '/' and '\'
	if (s) s++; else s = filename;
	appStrncpyz(buf, s, ARRAY_COUNT(buf));
	char *s2 = strchr(buf, '.');
	if (s2) *s2 = 0;
	appStrncpyz(Info.Name, buf, ARRAY_COUNT(Info.Name));
	Info.Package = this;

	unguardf(("%s", filename));
}


UnPackage::~UnPackage()
{
	// free resources
	if (Loader) delete Loader;
	int i;
	for (i = 0; i < Summary.NameCount; i++)
		free(NameTable[i]);
	delete NameTable;
	delete ImportTable;
	delete ExportTable;
#if UNREAL3
	if (DependsTable) delete DependsTable;
#endif
	// remove self from package table
	for (i = 0; i < PackageMap.Num(); i++)
		if (PackageMap[i].Package == this)
		{
			PackageMap.Remove(i);
			break;
		}
}


/*-----------------------------------------------------------------------------
	Loading particular import or export package entry
-----------------------------------------------------------------------------*/

UObject* UnPackage::CreateExport(int index)
{
	guard(UnPackage::CreateExport);

	// create empty object
	FObjectExport &Exp = GetExport(index);
	if (Exp.Object)
		return Exp.Object;

	const char *ClassName = GetObjectName(Exp.ClassIndex);
	UObject *Obj = Exp.Object = CreateClass(ClassName);
	if (!Obj)
	{
		printf("WARNING: Unknown class \"%s\" for object \"%s\"\n", ClassName, *Exp.ObjectName);
		return NULL;
	}
#if UNREAL3
	if (ArVer >= PACKAGE_V3 && (Exp.ExportFlags & EF_ForcedExport)) // ExportFlags appeared in ArVer=247
	{
		// find outermost package
		int PackageIndex = Exp.PackageIndex;
		if (PackageIndex)
		{
			while (true)
			{
				const FObjectExport &Exp2 = GetExport(PackageIndex - 1);
				if (!Exp2.PackageIndex) break;
				PackageIndex = Exp2.PackageIndex;
			}
			const FObjectExport &Exp2 = GetExport(PackageIndex - 1);
			assert(Exp2.ExportFlags & EF_ForcedExport);
			const char *PackageName = Exp2.ObjectName;
			printf("Forced export: %s'%s.%s'\n", ClassName, PackageName, *Exp.ObjectName);
		}
	}
#endif // UNREAL3
	UObject::BeginLoad();

	// setup constant object fields
	Obj->Package      = this;
	Obj->PackageIndex = index;
	Obj->Name         = Exp.ObjectName;
	// add object to GObjLoaded for later serialization
	UObject::GObjLoaded.AddItem(Obj);

	UObject::EndLoad();
	return Obj;

	unguardf(("%s:%d", Filename, index));
}


UObject* UnPackage::CreateImport(int index)
{
	guard(UnPackage::CreateImport);

	const FObjectImport &Imp = GetImport(index);

	// find root package
	int PackageIndex = Imp.PackageIndex;
	const char *PackageName = NULL;
	while (PackageIndex)
	{
		if (PackageIndex < 0)
		{
			const FObjectImport &Rec = GetImport(-PackageIndex-1);
			PackageIndex = Rec.PackageIndex;
			PackageName  = Rec.ObjectName;
		}
		else
		{
#if UNREAL3
			// possible for UE3 forced exports
			const FObjectExport &Rec = GetExport(PackageIndex-1);
			PackageIndex = Rec.PackageIndex;
			PackageName  = Rec.ObjectName;
#else
			appError("Wrong package index: %d", PackageIndex);
#endif // UNREAL3
		}
	}
	// load package
	UnPackage *Package = LoadPackage(PackageName);

	if (!Package)
	{
//		printf("WARNING: Import(%s): package %s was not found\n", *Imp.ObjectName, PackageName);
		return NULL;
	}
	//!! use full object path
	// find object in loaded package export table
	int NewIndex = Package->FindExport(Imp.ObjectName, Imp.ClassName);
	if (NewIndex == INDEX_NONE)
	{
		printf("WARNING: Import(%s) was not found in package %s\n", *Imp.ObjectName, PackageName);
		return NULL;
	}
	// create object
	return Package->CreateExport(NewIndex);

	unguardf(("%s:%d", Filename, index));
}


/*-----------------------------------------------------------------------------
	Searching for package and maintaining package list
-----------------------------------------------------------------------------*/

TArray<UnPackage::PackageEntry> UnPackage::PackageMap;
TArray<char*>					MissingPackages;

UnPackage *UnPackage::LoadPackage(const char *Name)
{
	guard(UnPackage::LoadPackage);

	int i;
	// check in loaded packages list
	for (i = 0; i < PackageMap.Num(); i++)
		if (!stricmp(Name, PackageMap[i].Name))
			return PackageMap[i].Package;
	// check missing packages
	for (i = 0; i < MissingPackages.Num(); i++)
		if (!stricmp(Name, MissingPackages[i]))
			return NULL;

	if (const CGameFileInfo *info = appFindGameFile(Name))
		return new UnPackage(info->RelativeName, appCreateFileReader(info));
	// package is missing
	printf("WARNING: package %s was not found\n", Name);
	MissingPackages.AddItem(strdup(Name));
	return NULL;

	unguardf(("%s", Name));
}
