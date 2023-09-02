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
					dense storage(We know schema that don't need mark type):
						int 4B,double 8B,string length+(1,2,3...)B
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
		...
		
	RDA now means offset of RecordHeader to DataStart, rather than block index, that it can provide more infomation with original info included;
	How about put metadata except Header to file end? That we dont't need to Move head records considering metadata is always rewrite;
*/

#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>

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
	using VIT=Uint32;
	using Timestamp=Uint64;
	
	constexpr LBA BadLBA=(LBA)-1;
	constexpr RDA BadRDA=(RDA)-1;
	constexpr VIT BadVIT=(VIT)-1;
	
	enum ERR
	{
		ERR_None=0,
		ERR_Unknown,
		ERR_TODO,
		ERR_OutOfMemory,
		ERR_FileIsNULL,
		ERR_CannotReadFileToNullptr,
		ERR_CannotWriteFileFromNullptr,
		ERR_ReadFileSeekError,
		ERR_WriteFileSeekError,
		ERR_FailedToReadFile,
		ERR_FailedToWriteFile,
		ERR_TargetIndexOrderError,
	};

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
		
		enum
		{
			RecordLayout_Single,
			RecordLayout_Append,
			RecordLayout_Mixed,
			RecordLayout_Dense,
		};
		
		Uint8 Magic[8]={'V','D','B','F','I','L','E','\0'};
		Uint32 Version=1;
		Uint32 HeaderSize;
		LBA AllocatorDataPos,
			TargetIndexTablePos,
			TableSchemaPos,
			ExtraMetadata0Pos,
			ExtraMetadata1Pos,
			DataRegionPos;
		Uint64 AllocatorDataSectors,
			   TargetIndexTableSectors,
			   TableSchemaSectors,
			   ExtraMetadata0Sectors,
			   ExtraMetadata1Sectors,
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
		Uint32 TableNameLength;
		Uint32 CommentLength;
		char TableName[MaxTableNameLength];
		char Comment[MaxCommentLength];
		
		Uint8 Padding[124];
		Uint8 MagicEnd[8]={'F','I','L','E','E','N','D','\0'};
	}__attribute__((packed));
	
	struct SequentialAllocatorHeader
	{
		Uint64 TotalBlocks;
	}__attribute__((packed));
	
	struct SchemaHeader
	{
		enum
		{
			Type_None=0,
			Type_String=1,
			Type_Int=2,
			Type_Double=3,
		};
		
		struct NameInfo
		{
			Uint32 len;
		}__attribute__((packed));
		
		Uint32 SchemaDataSize;
		Uint32 Columns;
		Uint32 TypeInfoOffset,
			   NameInfoOffset,
			   NameDataOffset;
	}__attribute__((packed));
	
	struct TargetIndex
	{
		enum {MaxVinLength=16};
		
		char VIN[MaxVinLength];
		VIT VIndex;
		Uint8 Padding1[12];
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
		enum {MaxLeafNums=(BlockSize-sizeof(RecordIndexHeader))/sizeof(TimestampToRecord)};
		
		TimestampToRecord mp[MaxLeafNums];
	}__attribute__((packed));
	
	struct RecordIndexNonleaf
	{
		enum {MaxNonleafNums=(BlockSize-sizeof(RecordIndexHeader))/sizeof(TimestampRangeToRecords)};
		
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
		VIT VIndex;
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
	
	struct BpTreeNode
	{
		enum {RecordsPerNode=(BlockSize-16)/sizeof(TimestampToRecord)};
		
		Uint8 Rank;
		Uint8 Padding[15];
		TimestampToRecord Records[RecordsPerNode];
	}__attribute__((packed));
	
	struct BpTreeLeaf
	{
		enum {RecordsPerLeaf=(BlockSize-sizeof(RDA)-8)/sizeof(TimestampToRecord)};
		
		RDA Next;
		Uint8 Padding[8];
		TimestampToRecord Records[RecordsPerLeaf];
	}__attribute__((packed));
	
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
		assert(sizeof(RecordIndexLeaf)+sizeof(RecordIndexHeader)<=BlockSize);
		assert(sizeof(RecordIndexNonleaf)+sizeof(RecordIndexHeader)<=BlockSize);
		assert(sizeof(RecordIndex)==ClustorSize);
		assert(sizeof(RecordHeader)==24);
		assert(sizeof(Property)==8);
		assert(sizeof(BpTreeNode)==BlockSize);
		assert(sizeof(BpTreeLeaf)==BlockSize);
	}
	
	#define BANCOPY(ClassName)								\
		ClassName(const ClassName &)=delete;				\
		ClassName(ClassName &&)=delete;						\
		ClassName& operator = (const ClassName &)=delete;	\
		ClassName& operator = (ClassName &&)=delete;
	
	class FileAccess
	{
		protected:
			FILE *f=nullptr;
			
		public:
			
			ERR Read(void *dst,Uint64 p,Uint64 len)
			{
				if (dst==nullptr)
					return ERR_CannotReadFileToNullptr;
				if (fseek(f,p,SEEK_SET))
					return ERR_ReadFileSeekError;
				Sint64 re=fread(dst,len,1,f);
				if (re!=1)
					return ERR_FailedToReadFile;
				return ERR_None;
			}
			
			ERR Write(void *src,Uint64 p,Uint64 len)
			{
				if (dst==nullptr)
					return ERR_CannotWriteFileToNullptr;
				if (fseek(f,p,SEEK_SET))
					return ERR_WriteFileSeekError;
				Sint64 re=fwrite(src,len,1,f);
				if (re!=1)
					return ERR_FailedToWriteFile;
				return ERR_None;
			}
			
			ERR ReadSector(Sector *sec,LBA p,int cnt=1)
			{return Read(sec,p*SectorSize,SectorSize*cnt);}
			
			ERR WriteSector(Sector *sec,LBA p,int cnt=1)
			{return Write(sec,p*SectorSize,SectorSize*cnt);}
			
			~FileAccess()
			{
				fflush(f);
			}
			
			FileAccess(FILE *_f):f(_f) {}
	};
	
	class DataCache
	{
		BANCOPY(DataCache);
		protected:
		
		public:
			
	};
	
	class DataCacheManager
	{
		BANCOPY(DataCacheManager);
		protected:
			
			
		public:
			
			ERR Require(RDA i)//Require a block, it will be usable after this call
			{
				//...
				return ERR_TODO;
			}
			
			ERR Release(RDA i)//Release a block, it may be unusable after this call
			{
				//...
				return ERR_TODO;
			}
			
			ERR Dirty(RDA i)//Mark a block as dirty, it will be write back to file when kickout from memory
			{
				//...
				return ERR_TODO;
			}
			
			ERR FlushAll()
			{
				//...
				return ERR_TODO;
			}
	};
	
	class RecordRW
	{
		public:
			TableHeader *super=nullptr;
			void *data=nullptr;
			
			//...
	};
	
	class DataIndexingManager
	{
		protected:
			
			//for B+ Tree: Maintain
		public:
			
//			Find,
//			Insert
	};
	
	class MetadataRegionRW
	{
		protected:
			FileAccess *F=nullptr;
			
			virtual ERR ParseRawData(void *data,Uint64 ud0,void *ud1)=0;
			virtual ERR EncodeRawData(void *data,Uint64 ud0,void *ud1)=0;
			
		public:
			virtual Uint64 EstimateSectors()=0;
			
			virtual ERR ReadFile(LBA start,Uint64 len,Uint64 ud0=0,void* ud1=nullptr)//[Start,end)
			{
				ERR e=ERR_None;
				Sector *data=new Sector[len];
				if (data==nullptr)
					return ERR_OutOfMemory;
				if (e=F->ReadSector(data,start,len))
					goto LABEL_DeleteData;
				if (e=ParseRawData(data,ud0,ud1))
					goto LABEL_DeleteData;
			LABEL_DeleteData;
				delete[] data;
				return e;
			}
			
			virtual ERR WriteFile(LBA start,Uint64 len,Uint64 ud0=0,void* ud1=nullptr)
			{
				ERR e=ERR_None;
				Sector *data=new Sector[len];
				if (data==nullptr)
					return ERR_OutOfMemory;
				if (e=EncodeRawData(data,ud0,ud1))
					goto LABEL_DeleteData;
				if (e=F->WriteSector(data,start,len))
					goto LABEL_DeleteData;
			LABEL_DeleteData;
				delete[] data;
				return e;
			}
			
			void SetFile(FileAccess *f)
			{F=f;}
	};
	
	class TableSchema:public MetadataRegionRW
	{
		public:
			struct SchemaColumn
			{
				std::string name;
				Uint8 type;
				int index;
				
				SchemaColumn(const char *name_s,const char *name_e,Uint8 _type,int idx)
				:name(name_s,name_e-name_s),type(_type),index(idx) {}
				
				SchemaColumn(const std::string &_name,Uint8 _type,int idx=-1):name(_name),type(_type),index(idx) {}
			};
		
		protected:
			std::vector <SchemaColumn> Schema;
			std::map <std::string,int> NameToIdx;
			Uint32 Columns;
			Uint32 TotalNameLength;
			
			Uint32 TypeInfoOffset()
			{return sizeof(SchemaHeader);}
			
			Uint32 NameInfoOffset()
			{return (TypeInfoOffset()+Columns*sizeof(Uint8)+7)/8*8;}
			
			Uint32 NameDataOffset()
			{return NameInfoOffset()+Columns*sizeof(SchemaHeader::NameInfo);}

			Uint32 SchemaDataSize()
			{return NameDataOffset()+TotalNameLength;}			
			
			void Clear()
			{
				Schema.clear();
				NameToIdx.clear();
				Columns=0;
				TotalNameLength=0;
			}
			
			virtual ERR ParseRawData(void *data,Uint64,void*)
			{
				Clear();
				SchemaHeader *header=(SchemaHeader*)data;
				Columns=header->Columns;
				Uint8 *Type=(Uint8*)(data+header->TypeInfoOffset);
				SchemaHeader::NameInfo *NI=(SchemaHeader::NameInfo*)(data+header->NameInfoOffset);
				char *Name=(char*)(data+header->NameDataOffset);
				
				Schema.reserve(Columns);
				for (int i=0;i<Columns;++i)
				{
					Schema.emplace_back(Name,Name+NI->len,*Type,i);
					NameToIdx[Schema.back().name]=i;
					TotalNameLength+=NI->len;
					Name+=NI->len;
					++NI;
					++Type;
				}
				assert(TotalNameLength==header->SchemaDataSize-header->NameDataOffset);
				return ERR_None;
			}
			
			virtual ERR EncodeRawData(void *data,Uint64,void*)
			{
				SchemaHeader *header=(SchemaHeader*)data;
				header->SchemaDataSize=SchemaDataSize();
				header->Columns=Columns;
				header->TypeInfoOffset=TypeInfoOffset();
				header->NameInfoOffset=NameInfoOffset();
				header->NameDataOffset=NameDataOffset();
				
				Uint8 *Type=(Uint8*)(data+header->TypeInfoOffset);
				SchemaHeader::NameInfo *NI=(SchemaHeader::NameInfo*)(data+header->NameInfoOffset);
				char *Name=(char*)(data+header->NameDataOffset);
				for (int i=0;i<Columns;++i)
				{
					SchemaColumn &c=Schema[i];
					*Type=c.type;
					*NI->len=c.name.length();
					memcpy(Name,c.name.data(),c.name.length());
					Name+=c.name.length();
				}
				assert((Uint8*)Name-data==header->SchemaDataSize);
				return ERR_None;
			}
			
		public:
			virtual Uint64 EstimateSectors()
			{return (SchemaDataSize()+SectorSize-1)/SectorSize;}
			
			ERR SetSchemaData(std::map<std::string,ColumnType> &columnTypeMap)
			{
				using namespace std;
				Clear();
				Schema.reserve(columnTypeMap.size());
				for (auto [name,type]:columnTypeMap)
					Schema.emplace_back(name,type);
				sort(Schema.begin(),Schema.end(),[](const SchemaColumn &x,const SchemaColumn &y)->bool
				{
					if (x.type==y.type)
						return x.name<y.name;
					else return x.type>y.type;
				});
				for (auto &vp:Schema)
				{
					vp.index=Columns;
					NameToIdx[vp.name]=Columns;
					++Columns;
				}
				return ERR_None;
			}
			
			std::string Name(int index)
			{return Schema[index].name;}
			
			Uint8 Type(int index)
			{return Schema[index].type;}
	};
		
	class TargetIndexManager:public MetadataRegionRW
	{
		BANCOPY(TargetIndexManager);
		public:
			struct TargetIndexData
			{
				std::string Name;
				VIT VIndex;
				RDA RecordIndex,
					LatestRecord;
				
				void WriteTargetIndex(TargetIndex *ti)
				{
					for (int i=0;i<Name.length()&&i<16;++i)
						ti->VIN[i]=Name[i];
					for (int i=Name.length();i<16;++i)
						ti->VIN[i]=0;
					ti->VIndex=VIndex;
					ti->RecordIndex=RecordIndex;
					ti->LatestRecord=LatestRecord;
				}
				
				TargetIndexData(TargetIndex *ti)
				{
					VIndex=ti->VIndex;
					ti->VIndex=0;
					Name=ti->VIN;
					ti->VIndex=VIndex;
					RecordIndex=ti->RecordIndex;
					LatestRecord=ti->LatestRecord;
				}
				
				TargetIndexData(const std::string &name,VIT vindex,RDA recordindex,RDA latestrecord)
				:Name(name),VIndex(vindex),RecordIndex(recordindex),LatestRecord(latestrecord) {}
			};
		
		protected:
			Uint32 TargetCounts=0;
			std::vector <TargetIndexData> Targets;
			std::map <std::string,VIT> NameToVIndex;
			
			void Clear()
			{
				TargetCounts=0;
				Targets.clear();
				NameToVIndex.clear();
			}
			
			virtual ERR ParseRawData(void *_data,Uint64 *targetcounts,void*)
			{
				Clear();
				TargetCounts=targetcounts;
				TargetIndex *data=(TargetIndex*)_data;
				Targets.reserve(TargetCounts); 
				for (int i=0;i<TargetCounts;++i)
				{
					if (data[i].VIndex!=i)//??
						return ERR_TargetIndexOrderError;
					Targets.emplace_back(data+i);
					NameToVIndex[Targets.back().Name]=i;
				}
				return ERR_None;
			}
			
			ERR EncodeRawData(TargetIndex *data,Uint64,void*)
			{
				for (int i=0;i<TargetCounts;++i)
					Targets[i].WriteTargetIndex(data+i);
				return ERR_None;
			}
			
		public:
			virtual Uint64 EstimateSectors()
			{
				static_assert(SectorSize%sizeof(TargetIndex)==0);
				return (TargetCounts*sizeof(TargetIndex)+SectorSize-1)/SectorSize;
			}
			
			ERR UpdateTargetInfo(VIT i,RDA recordindex,RDA latestrecord)
			{
				Targets[i].RecordIndex=recordindex;
				Targets[i].LatestRecord=latestrecord;
				return ERR_None;
			}
			
			RDA GetRecordIndex(VIT i)
			{return Targets[i].RecordIndex;}
			
			RDA GetLatestRecord(VIT i)
			{return Targets[i].LatestRecord;}
			
			VIT FindVIN(const std::string &VIN)
			{
				auto mp=NameToVIndex.find(VIN);
				if (mp==NameToVIndex.end())
					return BadVIT;
				else return mp->second;
			}
			
			VIT CreateNewVIN(const std::string &VIN,RDA recordindex=BadRDA,RDA latestrecord=BadRDA)
			{
				//assert(NameToVIndex.find(VIN)==NameToVIndex.end())??
				VIT re=TargetCounts++;
				Targets.emplace_back(VIN,re,recordindex,latestrecord);
				NameToVIndex[VIN]=re;
				return re;
			}
	};
	
	class BlockAllocator:public MetadataRegionRW
	{
		BANCOPY(BlockAllocator);
		protected:
			SequentialAllocatorHeader header;

			virtual ERR ParseRawData(void *data,Uint64,void*)
			{
				memcpy(&header,data,sizeof(header));
				return ERR_None;
			}
			
			virtual ERR EncodeRawData(void *data,Uint64,void*)
			{
				memcpy(data,&header,sizeof(header));
				return ERR_None;
			}
			
		public:
			virtual Uint64 EstimateSectors()
			{return 1;}
			
			Uint64 TotalBlocks()
			{return header.TotalBlocks;}
			
			RDA Allocate(int blocknum=1)
			{
				RDA re=header.TotalBlocks;
				header.TotalBlocks+=blocknum;
				return re;
			}
	};
	
	class DBEngine
	{
		BANCOPY(DBEngine);
		protected:
			FileAccess *F=nullptr;
			TableHeader *header=nullptr;//It will be updated realtime
			BlockAllocator allocator;
			TableSchema schema;
			TargetIndexManager tim;
			DataCacheManager cache;
			DataIndexingManager dim;
			
		public:
			
			VIT VinToVIndex(const string &vin)
			{return tim.FindVIN(vin);}
			
			RDA GetLatestRecordAddressOf(VIT v)
			{return tim.GetLatestRecord(v);}
			
			RDA GetRecordIndexAddressOf(VIT v)
			{return tim.GetRecordIndex(v);}
			
			Record* GetRecordFromRDA(RDA p)
			{
				//...
			}
			
			RecordIndex* GetRecordIndexFromRDA(RDA p)
			{
				//...
			}
			
			ERR RecordToRow(Record *rcd,Row &row)
			{
				//...
				return ERR_TODO;
			}
			
			ERR ReadFile()
			{
				ERR e=ERR_None;
				if (F==nullptr)
					return ERR_FileIsNULL;
				if (e=F->ReadSector((Sector*)header,0))
					return e;
				if (e=allocator->ReadFile(header->AllocatorDataPos,header->AllocatorDataSectors))
					return e;
				if (e=schema->ReadFile(header->TableSchemaPos,header->TableSchemaSectors))
					return e;
				if (e=tim->ReadFile(header->TargetIndexTablePos,header->TargetIndexTableSectors,header->TargetCounts))
					return e;
				return ERR_None;
			}
			
			ERR WriteFile()//header metadata should be up-to-date, otherwise, unpredictable error may appear
			{
				ERR e=ERR_None;
				if (F==nullptr)
					return ERR_FileIsNULL;
				if (e=F->WriteSector((Sector*)header,0))
					return e;
				if (e=allocator.WriteFile(header->AllocatorDataPos,header->AllocatorDataSectors));
					return e;
				if (e=schema->WriteFile(header->TableSchemaPos,header->TableSchemaSectors))
					return e;
				if (e=tim.WriteFile(header->TargetIndexTablePos,header->TargetIndexTableSectors))
					return e;
				return ERR_None;
			}
			
			~DBEngine()
			{
				//...
			}
			
			DBEngine(FILE *f):
			{
				using namespace std;
				F=new FileAccess(f);
				assert(F);
				header=new TableHeader();
				assert(header);
				allocator.SetFile(F);
				schema.SetFile(F);
				tim.SetFile(F);
			}
	};
};

#endif
