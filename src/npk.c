/*

	npk - General-Purpose File Packing Library
	See README for copyright and license information.

*/

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <time.h>
#include "npk.h"
#include "npk_dev.h"
#ifdef NPK_PLATFORM_MACOS 
#include <strings.h>
#else
#include <string.h>
#endif
#include "../external/tea/tea.h"
#include "../external/zlib/zlib.h"


NPK_API int		g_npkError = 0;	/* this variable has no multi-thread safety */
#ifdef NPK_PLATFORM_WINDOWS
int				g_useCriticalSection = 0;
#pragma warning( disable : 4996 )
#else
	#define strnicmp strncasecmp
	#define stricmp strcasecmp
#endif

NPK_CALLBACK	g_callbackfp;
NPK_SIZE		g_callbackSize;

NPK_PACKAGE	npk_package_open_with_fd( NPK_CSTR name, int fd, long offset, long size, NPK_TEAKEY teakey[4] )
{
	NPK_PACKAGEBODY*	pb = NULL;
	NPK_RESULT			res;

	res = npk_package_alloc( (NPK_PACKAGE*)&pb, teakey );
	if( res != NPK_SUCCESS )
		return NULL;

	if( npk_package_init( pb ) != NPK_SUCCESS )
		goto npk_package_open_return_null_with_free;

	pb->handle_ = fd;
	pb->usingFdopen_ = true;
	pb->offsetJump_ = offset;

	npk_seek( fd, offset, SEEK_CUR );
	
	res = __npk_package_open( pb, name, size, teakey );
	if( res != NPK_SUCCESS )
		goto npk_package_open_return_null_with_free;

	return (NPK_PACKAGE*)pb;

npk_package_open_return_null_with_free:
	if( pb )
		if( pb->handle_ > 0 )
			npk_close( pb->handle_ );

	NPK_SAFE_FREE( pb );
	return NULL;
}

NPK_PACKAGE npk_package_open( NPK_CSTR filename, NPK_TEAKEY teakey[4] )
{
	NPK_PACKAGEBODY*	pb = NULL;
	NPK_RESULT			res;

	res = npk_package_alloc( (NPK_PACKAGE*)&pb, teakey );
	if( res != NPK_SUCCESS )
		return NULL;

	if( npk_package_init( pb ) != NPK_SUCCESS )
		goto npk_package_open_return_null_with_free;

	if( npk_open( &pb->handle_, filename, false, false ) != NPK_SUCCESS )
		goto npk_package_open_return_null_with_free;
	
	res = __npk_package_open( pb, filename, 0, teakey );
	if( res != NPK_SUCCESS )
		goto npk_package_open_return_null_with_free;

	return (NPK_PACKAGE*)pb;

npk_package_open_return_null_with_free:
	if( pb )
		if( pb->handle_ > 0 )
			npk_close( pb->handle_ );

	NPK_SAFE_FREE( pb );
	return NULL;
}

bool npk_package_close( NPK_PACKAGE package )
{
	NPK_PACKAGEBODY* pb = (NPK_PACKAGEBODY*)package;
	NPK_RESULT res;
	int i;

	if( !package )
	{
		npk_error( NPK_ERROR_PackageIsNull );
		return false;
	}

	res = npk_package_remove_all_entity( pb );
	if( NPK_SUCCESS != res )
		return res;

#ifdef NPK_PLATFORM_WINDOWS
	DeleteCriticalSection( &pb->cs_ );
#endif

	if( false == pb->usingFdopen_ )
		npk_close( pb->handle_ );

	for( i = 0; i < NPK_HASH_BUCKETS; ++i )
		NPK_SAFE_FREE( pb->bucket_[i] );

	NPK_SAFE_FREE( pb );
	return true;
}

NPK_ENTITY npk_package_get_entity( NPK_PACKAGE package, NPK_CSTR entityname )
{
	NPK_ENTITYBODY* eb = NULL;
	NPK_PACKAGEBODY* pb = package;
	NPK_BUCKET* bucket = NULL;
	int bno = -1;

	if( !package )
	{
		npk_error( NPK_ERROR_PackageIsNull );
		return NULL;
	}

	if( pb->usingHashmap_ )
	{
		bucket = pb->bucket_[npk_get_bucket(entityname)];
		if( bucket != NULL )
		{
			eb = bucket->pEntityHead_;
			while( eb != NULL )
			{
#ifdef NPK_CASESENSITIVE
				if( strcmp( eb->name_, entityname ) == 0 )
#else
				if( stricmp( eb->name_, entityname ) == 0 )
#endif
				{
					pb->pEntityLatest_ = eb;
					return eb;
				}
				eb = eb->nextInBucket_;
			}
		}
	}
	else /* not usingHashmap_ */
	{
		eb = pb->pEntityHead_;
		while( eb != NULL )
		{
#ifdef NPK_CASESENSITIVE
			if( strcmp( eb->name_, entityname ) == 0 )
#else
			if( stricmp( eb->name_, entityname ) == 0 )
#endif
			{
				pb->pEntityLatest_ = eb;
				return eb;
			}
			eb = eb->next_;
		}

	}
	npk_error( NPK_ERROR_CannotFindEntity );
	return NULL;
}

NPK_SIZE npk_entity_get_size( NPK_ENTITY entity )
{
	NPK_ENTITYBODY* eb = entity;
	if( !entity )
	{
		npk_error( NPK_ERROR_EntityIsNull );
		return 0;
	}
	return eb->info_.originalSize_;
}

NPK_CSTR npk_entity_get_name( NPK_ENTITY entity )
{
	NPK_ENTITYBODY* eb = entity;
	if( !entity )
	{
		npk_error( NPK_ERROR_EntityIsNull );
		return 0;
	}
	return eb->name_;
}

bool npk_entity_read( NPK_ENTITY entity, void* buf )
{
	NPK_ENTITYBODY* eb = entity;
	NPK_PACKAGEBODY* pb = NULL;
	void** lplpTarget = &buf;
	void* lpDecompressBuffer = NULL;
	//NPK_SIZE uncompLen = 0;
	unsigned long uncompLen = 0;
	NPK_RESULT res;

	if( !entity )
	{
		npk_error( NPK_ERROR_EntityIsNull );
		return false;
	}

	if( eb->info_.flag_ & NPK_ENTITY_COMPRESS )
	{
		lpDecompressBuffer = malloc( sizeof(char) * eb->info_.size_ );
		lplpTarget = &lpDecompressBuffer;
	}

	pb = eb->owner_;
#ifdef NPK_PLATFORM_WINDOWS
	if( g_useCriticalSection )
		EnterCriticalSection( &pb->cs_ );
#endif
	npk_seek( pb->handle_, (long)eb->info_.offset_+pb->offsetJump_, SEEK_SET );

	res = npk_read( pb->handle_,
					(*lplpTarget),
					eb->info_.size_,
					g_callbackfp,
					NPK_PROCESSTYPE_ENTITY,
					g_callbackSize,
					eb->name_ );
#ifdef NPK_PLATFORM_WINDOWS
	if( g_useCriticalSection )
		LeaveCriticalSection( &pb->cs_ );
#endif

	if( res != NPK_SUCCESS )
		goto npk_entity_read_return_null_with_free;

	// Decode before uncompress, after v21
	if( ( eb->info_.flag_ & NPK_ENTITY_ENCRYPT ) && ( eb->info_.flag_ & NPK_ENTITY_REVERSE ) )
		tea_decode_buffer((char*)(*lplpTarget), eb->info_.size_, pb->teakey_);

	if( eb->info_.flag_ & NPK_ENTITY_COMPRESS )
	{
		uncompLen = eb->info_.originalSize_;

		if( uncompLen >= NPK_MIN_SIZE_ZIPABLE )
		{
#ifdef Z_PREFIX
			if( Z_OK != z_uncompress((Bytef*)(buf), (z_uLong*)&uncompLen, (const Bytef*)lpDecompressBuffer, (z_uLong)eb->info_.size_ ) )
#else
			if( Z_OK != uncompress((Bytef*)(buf), (uLong*)&uncompLen, (const Bytef*)lpDecompressBuffer, (uLong)eb->info_.size_ ) )
#endif
			{
				npk_error( NPK_ERROR_FailToDecompress );
				goto npk_entity_read_return_null_with_free;
			}

			if( eb->info_.originalSize_ != uncompLen )
			{
				npk_error( NPK_ERROR_FailToDecompress );
				goto npk_entity_read_return_null_with_free;
			}
		}
		else
			memcpy( buf, lpDecompressBuffer, eb->info_.size_ );

		NPK_SAFE_FREE( lpDecompressBuffer );
		lplpTarget = &buf;
	}

	// Decode after uncompress, before v21
	if( ( eb->info_.flag_ & NPK_ENTITY_ENCRYPT ) && !( eb->info_.flag_ & NPK_ENTITY_REVERSE ) )
		tea_decode_buffer((char*)(*lplpTarget), eb->info_.originalSize_, pb->teakey_);

	return true;

npk_entity_read_return_null_with_free:
	NPK_SAFE_FREE( lpDecompressBuffer );
	return false;
}

bool npk_entity_read_partial( NPK_ENTITY entity, void* buf, NPK_SIZE offset, NPK_SIZE size )
{
	NPK_ENTITYBODY* eb = entity;
	NPK_PACKAGEBODY* pb = NULL;
	NPK_RESULT res;

	if( !entity )
	{
		npk_error( NPK_ERROR_EntityIsNull );
		return false;
	}

	if( eb->info_.flag_ & ( NPK_ENTITY_COMPRESS | NPK_ENTITY_ENCRYPT ) )
	{
		npk_error( NPK_ERROR_CantReadCompressOrEncryptEntityByPartial );
		return false;
	}

	pb = eb->owner_;
#ifdef NPK_PLATFORM_WINDOWS
	if( g_useCriticalSection )
		EnterCriticalSection( &pb->cs_ );
#endif
	npk_seek( pb->handle_, (long)(eb->info_.offset_ + offset)+pb->offsetJump_, SEEK_SET );

	res = npk_read( pb->handle_,
					buf,
					size,
					g_callbackfp,
					NPK_PROCESSTYPE_ENTITY,
					g_callbackSize,
					eb->name_ );
#ifdef NPK_PLATFORM_WINDOWS
	if( g_useCriticalSection )
		LeaveCriticalSection( &pb->cs_ );
#endif

	if( res != NPK_SUCCESS )
		return false;

	return true;
}

void npk_enable_callback( NPK_CALLBACK cb, NPK_SIZE cb_size )
{
	g_callbackfp = cb;
	g_callbackSize = cb_size;
}

void npk_disable_callback()
{
	g_callbackfp = NULL;
}

#ifdef NPK_PLATFORM_WINDOWS
void npk_enable_criticalsection()
{
	g_useCriticalSection = 1;
}

void npk_disable_criticalsection()
{
	g_useCriticalSection = 0;
}
#endif

NPK_ENTITY npk_package_get_first_entity( NPK_PACKAGE package )
{
	NPK_PACKAGEBODY* pb = package;

	if( !package )
	{
		npk_error( NPK_ERROR_PackageIsNull );
		return NULL;
	}

	return pb->pEntityHead_;
}

NPK_ENTITY npk_entity_next( NPK_ENTITY entity )
{
	NPK_ENTITYBODY* eb = entity;

	if( !entity )
	{
		npk_error( NPK_ERROR_EntityIsNull );
		return NULL;
	}

	return eb->next_;
}

NPK_RESULT __npk_package_open( NPK_PACKAGEBODY* pb, const NPK_CHAR* filename, long filesize, NPK_TEAKEY teakey[4] )
{
	NPK_CHAR			buf[512];
	NPK_ENTITYBODY*		eb = NULL;
	NPK_ENTITYINFO_V21	oldinfo;
	NPK_SIZE			entityCount	= 0;
	NPK_CHAR*			entityheaderbuf;
	NPK_CHAR*			pos;
	long				entityheadersize = 0;
	NPK_RESULT			res;

	// Read common header
	res = npk_read( pb->handle_,
					(void*)&pb->info_,
					sizeof(NPK_PACKAGEINFO),
					g_callbackfp,
					NPK_PROCESSTYPE_PACKAGEHEADER,
					g_callbackSize,
					filename );
	if( res != NPK_SUCCESS ) return res;

	if( strncmp( pb->info_.signature_, NPK_SIGNATURE, 4 ) != 0 )
		if( strncmp( pb->info_.signature_, NPK_OLD_SIGNATURE, 4 ) != 0 )
			return( npk_error( NPK_ERROR_NotValidPackage ) );

	// version 18 / read own tea key
	if( pb->info_.version_ < NPK_VERSION_REFACTORING )
	{
		return ( npk_error( NPK_ERROR_NotSupportedVersion ) );
	}
	else
	{
		if( teakey == NULL )
		{
			return ( npk_error( NPK_ERROR_NeedSpecifiedTeaKey ) );
		}
		memcpy( pb->teakey_, teakey, sizeof(NPK_TEAKEY) * 4 );
	}

	// version 23 / package timestamp
	if( pb->info_.version_ >= NPK_VERSION_PACKAGETIMESTAMP )
	{
		res = npk_read( pb->handle_,
						(void*)&pb->modified_,
						sizeof(NPK_TIME),
						g_callbackfp,
						NPK_PROCESSTYPE_PACKAGEHEADER,
						g_callbackSize,
						filename );
		if( res != NPK_SUCCESS ) return res;
	}

	entityCount = pb->info_.entityCount_;
	pb->info_.entityCount_ = 0;

	if( pb->info_.version_ >= NPK_VERSION_SINGLEPACKHEADER )
	{
		if( filesize == 0 )
			filesize = npk_seek( pb->handle_, 0, SEEK_END );
		entityheadersize = filesize - (long)pb->info_.entityInfoOffset_;
		npk_seek( pb->handle_, (long)pb->info_.entityInfoOffset_+pb->offsetJump_, SEEK_SET );

		entityheaderbuf = malloc( entityheadersize );
		if( !entityheaderbuf )
		{
			return( npk_error( NPK_ERROR_NotEnoughMemory ) );
		}

		res = npk_read_encrypt( teakey,
								pb->handle_,
								(void*)entityheaderbuf,
								entityheadersize,
								g_callbackfp,
								NPK_PROCESSTYPE_ENTITYHEADER,
								g_callbackSize,
								filename );
		if( res != NPK_SUCCESS ) return res;

		pos = entityheaderbuf;
		
		while( entityCount > 0 )
		{
			--entityCount;

			res = npk_entity_alloc( (NPK_ENTITY*)&eb );
			if(	res != NPK_SUCCESS )
				goto __npk_package_open_return_res_with_free;

			eb->owner_ = pb;
			memcpy( &eb->info_, pos, sizeof(NPK_ENTITYINFO) );
			pos += sizeof(NPK_ENTITYINFO);

			if( eb->info_.offset_ >= pb->info_.entityInfoOffset_ )
			{
				res = npk_error( NPK_ERROR_InvalidTeaKey );
				goto __npk_package_open_return_res_with_free;
			}

			eb->newflag_ = eb->info_.flag_;
			eb->name_ = malloc( sizeof(NPK_CHAR)*(eb->info_.nameLength_+1) );
			if( !eb->name_ )
			{
				res = npk_error( NPK_ERROR_NotEnoughMemory );
				goto __npk_package_open_return_res_with_free;
			}
			eb->name_[eb->info_.nameLength_] = '\0';
			memcpy( eb->name_, pos, eb->info_.nameLength_ );
			pos += eb->info_.nameLength_;

			__npk_package_add_entity( pb, eb, false );
		}
		NPK_SAFE_FREE( entityheaderbuf );
	}
	else	// old style entity header
	{
		npk_seek( pb->handle_, (long)pb->info_.entityInfoOffset_+pb->offsetJump_, SEEK_SET );
		while( entityCount > 0 )
		{
			--entityCount;

			res = npk_entity_alloc( (NPK_ENTITY*)&eb );
			if(	res != NPK_SUCCESS )
				goto __npk_package_open_return_res_with_free;

			eb->owner_ = pb;

			// read entity info
			if( pb->info_.version_ < NPK_VERSION_UNIXTIMESUPPORT )
			{
				res = npk_read_encrypt( teakey,
										pb->handle_,
										(void*)&oldinfo,
										sizeof(NPK_ENTITYINFO),
										g_callbackfp,
										NPK_PROCESSTYPE_ENTITYHEADER,
										g_callbackSize,
										filename );
				if( res != NPK_SUCCESS )
					goto __npk_package_open_return_res_with_free;

				eb->info_.offset_ = oldinfo.offset_;
				eb->info_.size_ = oldinfo.size_;
				eb->info_.originalSize_ = oldinfo.originalSize_;
				eb->info_.flag_ = oldinfo.flag_;
				npk_filetime_to_unixtime( &oldinfo.modified_, &eb->info_.modified_ );
				eb->info_.nameLength_ = oldinfo.nameLength_;
			}
			else
			{
				res = npk_read_encrypt( teakey,
										pb->handle_,
										(void*)&eb->info_,
										sizeof(NPK_ENTITYINFO),
										g_callbackfp,
										NPK_PROCESSTYPE_ENTITYHEADER,
										g_callbackSize,
										filename );
				if( res != NPK_SUCCESS )
					goto __npk_package_open_return_res_with_free;
			}

			if( eb->info_.offset_ >= pb->info_.entityInfoOffset_ )
			{
				res = npk_error( NPK_ERROR_InvalidTeaKey );
				goto __npk_package_open_return_res_with_free;
			}

			
			res = npk_read_encrypt( teakey,
									pb->handle_,
									(void*)buf,
									sizeof(char) * eb->info_.nameLength_,
									g_callbackfp,
									NPK_PROCESSTYPE_ENTITYHEADER,
									g_callbackSize,
									filename );
			if( res != NPK_SUCCESS )
				goto __npk_package_open_return_res_with_free;

			eb->newflag_ = eb->info_.flag_;

			// copy name into entity body
			buf[eb->info_.nameLength_] = '\0';
			res = npk_alloc_copy_string( &eb->name_, buf );
			if( res != NPK_SUCCESS )
				goto __npk_package_open_return_res_with_free;

			__npk_package_add_entity( pb, eb, false );
		}
	}
	return NPK_SUCCESS;

__npk_package_open_return_res_with_free:

	NPK_SAFE_FREE( eb );
	return res;
}

