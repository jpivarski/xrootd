/*
 * XrdClXCpCtx_.cc
 *
 *  Created on: Jan 20, 2017
 *      Author: simonm
 */

#include "XrdCl/XrdClXCpCtx.hh"
#include "XrdCl/XrdClXCpSrc.hh"
#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClConstants.hh"

#include <algorithm>

namespace XrdCl
{

XCpCtx::XCpCtx( const std::vector<std::string> &urls, uint64_t blockSize, uint8_t parallelSrc, uint64_t chunkSize, uint64_t parallelChunks, int64_t fileSize ) :
      pUrls( std::deque<std::string>( urls.begin(), urls.end() ) ), pBlockSize( blockSize ),
      pParallelSrc( parallelSrc ), pChunkSize( chunkSize ), pParallelChunks( parallelChunks ),
      pOffset( 0 ), pFileSize( -1 ), pFileSizeCV( 0 ), pDataReceived( 0 ), pDone( false ),
      pDoneCV( 0 ), pRefCount( 1 )
{
  SetFileSize( fileSize );
}

XCpCtx::~XCpCtx()
{
  // at this point there's no concurrency
  // this object dies as the last one
  while( !pSink.IsEmpty() )
  {
    ChunkInfo *chunk = pSink.Get();
    if( chunk )
      XCpSrc::DeleteChunk( chunk );
  }
}

bool XCpCtx::GetNextUrl( std::string & url )
{
  XrdSysMutexHelper lck( pMtx );
  if( pUrls.empty() ) return false;
  url = pUrls.front();
  pUrls.pop();
  return true;
}

XCpSrc* XCpCtx::WeakestLink( XCpSrc *exclude )
{
  uint64_t transferRate = -1; // set transferRate to max uint64 value
  XCpSrc *ret = 0;

  std::list<XCpSrc*>::iterator itr;
  for( itr = pSources.begin() ; itr != pSources.end() ; ++itr )
  {
    XCpSrc *src = *itr;
    if( src == exclude ) continue;
    uint64_t tmp = src->TransferRate();
    if( src->HasData() && tmp < transferRate )
    {
      ret = src;
      transferRate = tmp;
    }
  }

  return ret;
}

void XCpCtx::PutChunk( ChunkInfo* chunk )
{
  pSink.Put( chunk );
}

std::pair<uint64_t, uint64_t> XCpCtx::GetBlock()
{
  XrdSysMutexHelper lck( pMtx );

  uint64_t blkSize = pBlockSize, offset = pOffset;
  if( pOffset + blkSize > uint64_t( pFileSize ) )
    blkSize = pFileSize - pOffset;
  pOffset += blkSize;

  return std::make_pair( offset, blkSize );
}

void XCpCtx::SetFileSize( int64_t size )
{
  XrdSysMutexHelper lck( pMtx );
  if( pFileSize < 0 && size >= 0 )
  {
    XrdSysCondVarHelper lck( pFileSizeCV );
    pFileSize = size;
    pFileSizeCV.Broadcast();

    if( pBlockSize > uint64_t( pFileSize ) / pParallelSrc )
      pBlockSize = pFileSize / pParallelSrc;

    if( pBlockSize < pChunkSize )
      pBlockSize = pChunkSize;
  }
}

XRootDStatus XCpCtx::Initialize()
{
  for( uint8_t i = 0; i < pParallelSrc; ++i )
  {
    XCpSrc *src = new XCpSrc( pChunkSize, pParallelChunks, pFileSize, this );
    pSources.push_back( src );
    src->Start();
  }

  if( pSources.empty() )
  {
    Log *log = DefaultEnv::GetLog();
    log->Error( UtilityMsg, "Failed to initialize (failed to create new threads)" );
    return XRootDStatus( stError, errInternal, EAGAIN, "XCpCtx: failed to create new threads." );
  }

  return XRootDStatus();
}

XRootDStatus XCpCtx::GetChunk( XrdCl::ChunkInfo &ci )
{
  // if we received all the data we are done here
  if( pDataReceived == uint64_t( pFileSize ) )
  {
    XrdSysCondVarHelper lck( pDoneCV );
    pDone = true;
    pDoneCV.Broadcast();
    return XRootDStatus( stOK, suDone );
  }

  // check if there are any active sources
  size_t nbRunning = 0;
  std::list<XCpSrc*>::iterator itr;
  for( itr = pSources.begin() ; itr != pSources.end() ; ++ itr)
    if( (*itr)->IsRunning() )
      ++nbRunning;

  // if we don't have active sources it means we failed
  if( nbRunning == 0 )
  {
    XrdSysCondVarHelper lck( pDoneCV );
    pDone = true;
    pDoneCV.Broadcast();
    return XRootDStatus( stError, errNoMoreReplicas );
  }

  ChunkInfo *chunk = pSink.Get();
  if( chunk )
  {
    pDataReceived += chunk->length;
    ci = *chunk;
    delete chunk;
    return XRootDStatus( stOK, suContinue );
  }

  return XRootDStatus( stOK, suRetry );
}

void XCpCtx::NotifyIdleSrc()
{
  pDoneCV.Broadcast();
}

bool XCpCtx::AllDone()
{
  XrdSysCondVarHelper lck( pDoneCV );

  if( !pDone )
    pDoneCV.Wait( 60 );

  return pDone;
}


} /* namespace XrdCl */
