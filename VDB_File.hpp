#ifndef VDB_FILE_HPP
#define VDB_FILE_HPP

/*
	Table:MetaData|Data
		MetaData is always cached in memory and maintained in better datastructure;
		MetaData:
			Header|AllocatorData|TargetIndexTable|ReservedData
			Header(512B or 4K? or more?):
				Magic(8B)|Version(8B)|HeaderSize(8B)|
					AllocatorDataLBA(8B)|TargetIndexTableLBA(8B)|DataLBA(8B)|
					AllocatorDataSecotrs(8B)|TargetIndexTableSectors(8B)|DataSectors(8B)|
					TargetCounts(8B)|SectorSize(4B)|ClustorSize(4B)|UsedDataClustors(8B)|FreeDataClustors(8B)|
					TableName(32B)|Comment(?B)|RecordLayout|Schema|Padding|MagicEnd(8B)
				Some of field can be 4B or less, but I'm lazy...
			AllocatorData:
				Considering that there is no delete operation, so it don't need to maintain free clustors?
				Some clustors may be moved where holes turn up, so we still need it?
			TargetIndexTable:Mapping string to index
				Can be cached to memory;
				Currently read all data to memory;
				TargetIndex0|TargetIndex1|TargetIndex2|...
				TargetIndex:
					VIN(16B)|VIndex(8B)|Padding(8B)|First RecordIndexRDA(8B)|LatestRecordRDA(8B)|Padding(16B)
			
		Data:Allocate in block?
			RecordIndex0|Record1|RecordIndex3|Record5|...
			Sizeof RecordIndex should equal to sizeof RecordData for addressing in RDA;
			RecordIndex:Mapping timestamp to index 
				RecordIndexHeader|RecordIndexData
				RecordIndexHeader:
					Rank(1B)|Padding(3B)|Counts(4B)
				if Rank is 0
					RecorIndexData:ts0->RDA0|ts1->RDA3|ts2->RDA1|...
						timestamp:8B -> RDA:8B(maybe 4B is enough,but 8B is easier for align)
				else if Rank >=1,means it is the Rank level
					RecordIndexData:[ts_start(8B),ts_end(8B)]->Next level RecordIndex RDA(8B)
						It needs maintain to keep balance;
				Cache this entire RecordIndex to memory that we can sort timestamp etc to improve performance;
				Use LRU etc to cache?
				Maintain?Use B+ Tree?
			Record:RecordHeader|Property0|Property1|...
				RecordHeader:Padding(4B)|VIndex(4B)|Timestamp(8B)|Offset(4B)|PropertySize(Properties and TailStrings)(4B)
					Reserve these data for reversed modification,such as moving this clustor to tail;
					It can also used for identify which record it belongs to when multi records in a same clustor?
					Offset is the offset of propertystart to headerstart;
					if noncontinuous storage is required, extra field preRDA and nxtRDA may be needed;
				Property:
					int:4B
					double:8B
					string:unknown
					may be store in fixed size cell?
						int padding to 8B
						string the first bytes means length
							if length<=7,then store localy
							else store in tail with original order
							if length==255,left bytes of that 8B will be used as length(Maybe 4 of them is enough?)
					dense storage:
						int 5B,double 9B,string length+(1,2,3...)B
			Record should padding to for example 512B, so a cluster can have 8 records;(?)
			if one record is too large, it can occupy nearby sector, so it can have 1024B etc;
			if one record is extremely large, it can occupy the clustor after it, similar to sector;
			When multi Record in one clustor:
				1:H0|P0-0|P0-1|..|TailStrings0|H1|P1-0|P1-1|...|TailString1|H2|...
				2:H0|H1|H2|...|P0-0|P0-1|...|P1-0|P1-1|...|P2-0|...|TailStrings0|TailStrings1|TailStrings2|...
				3:H0|H1|H2|...|P0-0|P0-1|...|TailStrings0|P1-0|...|TailStrings1|P2-0|...
				I think I would choose 3;
	AIB:Address in bytes, start from file begin
	LBA:Logical block(sector) address
	RDA:Record data address, start from the first record which indexed as 0
	
	UpdateOperation:
		Directly modify is OK;
	WriteOperaiton:
		Append Records in Data,
		maintain corespond RecordIndex,maybe TargetIndex and Header ...
	QueryRange:
		Read TargetIndex,
		Read RecordIndex Rank times,
		Read Record
	QueryLatest:
		Read TargetIndexs,
		Read Record
	MainTain:
		Move clustor data will cause index change,
*/

inline namespace VDB
{
	using Sint8=char;
	using Sint16=short;
	using Sint32=int;
	using Sint64=long long;
//	using Sint128=__int128_t;
	using Uint8=unsigned char; 
	using Uint16=unsigned short;
	using Uint32=unsigned int;
	using Uint64=unsigned long long;
//	using Uint128=__uint128_t;
	
	constexpr unsigned SectorSize=512;
	constexpr unsigned ClustorSize=4096;
	constexpr unsigned BlockSize=ClustorSize;

	using LBA=Uint64;
	using RDA=Uint64;
	using Timestamp=Uint64;

	struct Sector
	{
		Uint8 bytes[SectorSize];
	}__attribute__((packed));
	
	struct Clustor
	{
		enum {SectorsPerClustor=ClustorSize/SectorSize};
		
		Sector sec[SectorsPerClustor];
	}__attribute__((packed));

	struct TableHeader
	{
		enum
		{
			MaxTableNameLength=64,
			MaxCommentLength=128
		};
		
		Uint8 Magic[8]={'V','D','B','F','I','L','E','\0'};
		Uint32 Version=1;
		Uint32 HeaderSize;
		LBA AllocatorData,
			TargetIndexTable,
			DataRegion;
		Uint64 AllocatorDataSectors,
			   TargetIndexTableSectors,
			   DataRegionSectors;
		Uint32 SectorSize,
			   ClustorSize,
			   BlockSize;
		Uint32 TargetCounts;
		Uint64 UsedSectors,
			   FreeSectors;
		Uint64 UsedClustors,
			   FreeClustors;
		Uint64 UsedDataBlocks,
			   FreeDataBlocks;
		Uint32 RecordDataLayout;
		Uint32 TableSchemaOffsetToHeader;
		Uint32 ExtraDataOffsetToHeader0;
		Uint32 ExtraDataOffsetToHeader1;
		Uint32 TableNameLength;
		Uint32 CommentLength;
		char TableName[MaxTableNameLength];
		char Comment[MaxCommentLength];
		
		Uint8 Padding[160];
		Uint8 MagicEnd[8]={'F','I','L','E','E','N','D','\0'};
	}__attribute__((packed));
	
	class TableSchemaRW
	{
		public:
			enum
			{
				Type_None=0,
				Type_String=1,
				Type_Int=2,
				Type_Double=3,
			};
			
			struct NameInfo
			{
				Uint32 len,
					   offset;
			}__attribute__((packed));
			
			void *data=nullptr;
			Uint32 ThisDataSize;
			Uint32 Columns;
			Uint32 TypeInfoOffset;
			Uint32 NameInfoOffset;
			Uint32 NameDataOffset;
			
			Uint8 GetType(int col)
			{return *((Uint8*)data+TypeInfoOffset+col);}
			
			void WriteSchemaToData(/*XXX*/)
			{
				//...
			}
			
			void SetDataAndReadInfo(void *d)
			{	
				if (d==nullptr)
					return;
				data=d;
				ThisDataSize	=*((int*)data+0);
				Columns			=*((int*)data+1);
				TypeInfoOffset	=*((int*)data+2);
				NameInfoOffset	=*((int*)data+3);
				NameDataOffset	=*((int*)data+4);
			}
	};
	
	struct TargetIndex
	{
		enum {MaxVinLength=16};
		
		char VIN[MaxVinLength];
		Uint8 Padding1[12];
		Uint32 VIndex;
		RDA RecordIndex,
			LatestRecord;
		Uint8 Padding2[16];
	}__attribute__((packed));

	struct TargetIndexInSector
	{
		enum {TargetIndexsPerSector=SectorSize/sizeof(TargetIndex)};
		
		TargetIndex ti[TargetIndexsPerSector];
	}__attribute__((packed));
	
	struct TargetIndexInClustor
	{
		enum {TargetIndexsPerClustor=ClustorSize/sizeof(TargetIndex)};
		
		TargetIndex ti[TargetIndexsPerClustor];
	}__attribute__((packed));
	
	struct RecordIndexHeader
	{
		Uint8 Rank;
		Uint8 Padding[3];
		Uint32 Counts;
	}__attribute__((packed));

	struct TimestampToRecord
	{
		Timestamp t;
		RDA p;
	}__attribute__((packed));;
	
	struct TimestampRangeToRecords
	{
		Timestamp s,t;
		RDA p;
	};
	
	struct RecordIndexLeaf
	{
		enum {MaxLeafNums=(ClustorSize-sizeof(RecordIndexHeader))/sizeof(TimestampToRecord)};
		
		TimestampToRecord mp[MaxLeafNums];
	}__attribute__((packed));
	
	struct RecordIndexNonleaf
	{
		enum {MaxNonleafNums=(ClustorSize-sizeof(RecordIndexHeader))/sizeof(TimestampRangeToRecords)};
		
		TimestampRangeToRecords mp[MaxNonleafNums];
	}__attribute__((packed));
	
	struct RecordIndex
	{
		RecordIndexHeader header;
		union
		{
			RecordIndexLeaf leaf;
			RecordIndexNonleaf nonleaf;
		}__attribute__((packed));
		Uint8 Padding[ClustorSize-sizeof(RecordIndexHeader)-(sizeof(RecordIndexLeaf)>sizeof(RecordIndexNonleaf)?sizeof(RecordIndexLeaf):sizeof(RecordIndexNonleaf))];
	}__attribute__((packed));
	
	struct RecordHeader
	{
		Uint8 Padding[8];
		Uint32 VIndex;
		Timestamp ts;
		Uint32 Offset;
		Uint32 DataSize;
	}__attribute__((packed));
	
	struct Property
	{
		union
		{
			struct
			{
				Uint8 Padding1[4];
				Uint32 i;
			};

			double x;

			struct
			{
				Uint8 len;
				union
				{
					struct
					{
						Uint8 Padding2[3];
						Uint32 lenght;
					}__attribute__((packed));;
					char s[7];
				};
			}__attribute__((packed));;
		};
		Uint8 TailStrings[0];
	}__attribute__((packed));
	
	class RecordRW
	{
		public:
			TableHeader *super=nullptr;
			void *data=nullptr;
			
			
			
			//...
	};
	
	class FilePageFaulter
	{
		
	};
		
	class TargetIndexTable
	{
		public:
			
	};
	
	void AssertStructures()
	{
		using namespace std;
		assert(sizeof(TableHeader)==SectorSize);
		assert(sizeof(TargetIndex)==64);
		assert(sizeof(TargetIndexInSector)==SectorSize);
		assert(sizeof(TargetIndexInClustor)==ClustorSize);
		assert(sizeof(RecordIndexHeader)==8);
		assert(sizeof(TimestampToRecord)==16);
		assert(sizeof(TimestampRangeToRecords)==24);
		assert(sizeof(RecordIndexLeaf)+sizeof(RecordIndexHeader)<=ClustorSize);
		assert(sizeof(RecordIndexNonleaf)+sizeof(RecordIndexHeader)<=ClustorSize);
		assert(sizeof(RecordIndex)==ClustorSize);
		assert(sizeof(RecordHeader)==24);
		assert(sizeof(Property)==8);
	}
};

#endif
