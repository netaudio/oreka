/*
 * Oreka -- A media capture and retrieval platform
 * 
 * Copyright (C) 2005, orecx LLC
 *
 * http://www.orecx.com
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License.
 * Please refer to http://www.gnu.org/copyleft/gpl.html
 *
 */
#pragma warning( disable: 4786 )

#define _WINSOCKAPI_		// prevents the inclusion of winsock.h

#include "ConfigManager.h"
#include "BatchProcessing.h"
#include "ace/OS_NS_unistd.h"
#include "audiofile/LibSndFileFile.h"
#include "Daemon.h"
#include "Filter.h"

TapeProcessorRef BatchProcessing::m_singleton;

void BatchProcessing::Initialize()
{
	if(m_singleton.get() == NULL)
	{
		m_singleton.reset(new BatchProcessing());
		TapeProcessorRegistry::instance()->RegisterTapeProcessor(m_singleton);
	}
}


BatchProcessing::BatchProcessing()
{
	m_threadCount = 0;

	struct tm date = {0};
	time_t now = time(NULL);
	ACE_OS::localtime_r(&now, &date);
	m_currentDay = date.tm_mday;
}

CStdString __CDECL__ BatchProcessing::GetName()
{
	return "BatchProcessing";
}

TapeProcessorRef  BatchProcessing::Instanciate()
{
	return m_singleton;
}

void BatchProcessing::AddAudioTape(AudioTapeRef& audioTapeRef)
{
	if (!m_audioTapeQueue.push(audioTapeRef))
	{
		// Log error
		LOG4CXX_ERROR(LOG.batchProcessingLog, CStdString("queue full"));
	}
}

void BatchProcessing::SetQueueSize(int size)
{
	m_audioTapeQueue.setSize(size);
}

void BatchProcessing::ThreadHandler(void *args)
{
	CStdString debug;

	TapeProcessorRef batchProcessing = TapeProcessorRegistry::instance()->GetNewTapeProcessor(CStdString("BatchProcessing"));
	if(batchProcessing.get() == NULL)
	{
		LOG4CXX_ERROR(LOG.batchProcessingLog, "Could not instanciate BatchProcessing");
		return;
	}
	BatchProcessing* pBatchProcessing = (BatchProcessing*)(batchProcessing->Instanciate().get());

	pBatchProcessing->SetQueueSize(CONFIG.m_batchProcessingQueueSize);

	int threadId = 0;
	{
		MutexSentinel sentinel(pBatchProcessing->m_mutex);
		threadId = pBatchProcessing->m_threadCount++;
	}
	CStdString threadIdString = IntToString(threadId);
	debug.Format("thread Th%s starting - queue size:%d", threadIdString, CONFIG.m_batchProcessingQueueSize);
	LOG4CXX_INFO(LOG.batchProcessingLog, debug);

	bool stop = false;

	for(;stop == false;)
	{
		AudioFileRef fileRef;
		AudioFileRef outFileRef;

		try
		{
			AudioTapeRef audioTapeRef = pBatchProcessing->m_audioTapeQueue.pop();
			if(audioTapeRef.get() == NULL)
			{
				if(DaemonSingleton::instance()->IsStopping())
				{
					stop = true;
				}
			}
			else
			{
				fileRef = audioTapeRef->GetAudioFileRef();
				CStdString filename = audioTapeRef->GetFilename();

				// Let's work on the tape we have pulled
				//CStdString threadIdString = IntToString(threadId);
				LOG4CXX_INFO(LOG.batchProcessingLog, CStdString("Th") + threadIdString + " processing: " + audioTapeRef->GetIdentifier());

				fileRef->MoveOrig();
				fileRef->Open(AudioFile::READ);

				AudioChunkRef chunkRef;
				AudioChunkRef tmpChunkRef;

				switch(CONFIG.m_storageAudioFormat)
				{
				case FfUlaw:
					outFileRef.reset(new LibSndFileFile(SF_FORMAT_ULAW | SF_FORMAT_WAV));
					break;
				case FfAlaw:
					outFileRef.reset(new LibSndFileFile(SF_FORMAT_ALAW | SF_FORMAT_WAV));
					break;
				case FfGsm:
					outFileRef.reset(new LibSndFileFile(SF_FORMAT_GSM610 | SF_FORMAT_WAV));
					break;
				case FfPcmWav:
				default:
					outFileRef.reset(new LibSndFileFile(SF_FORMAT_PCM_16 | SF_FORMAT_WAV));
				}

				FilterRef filter;
				FilterRef decoder1;
				FilterRef decoder2;

				bool firstChunk = true;
				bool voIpSession = false;

				while(fileRef->ReadChunkMono(chunkRef))
				{
					AudioChunkDetails details = *chunkRef->GetDetails();
					if(firstChunk && details.m_rtpPayloadType != -1)
					{
						CStdString rtpPayloadType = IntToString(details.m_rtpPayloadType);
						LOG4CXX_INFO(LOG.batchProcessingLog, CStdString("Th") + threadIdString + " RTP payload type:" + rtpPayloadType);

						CStdString filterName("RtpMixer");
						filter = FilterRegistry::instance()->GetNewFilter(filterName);
						if(filter.get() == NULL)
						{
							debug = "Could not instanciate RTP mixer";
							throw(debug);
						}
						decoder1 = FilterRegistry::instance()->GetNewFilter(details.m_rtpPayloadType);
						decoder2 = FilterRegistry::instance()->GetNewFilter(details.m_rtpPayloadType);
						if(decoder1.get() == NULL || decoder2.get() == NULL)
						{
							debug.Format("Could not find decoder for RTP payload type:%u", chunkRef->GetDetails()->m_rtpPayloadType);
							throw(debug);
						}
						voIpSession = true;
					}
					if(firstChunk)
					{
						firstChunk = false;

						// At this point, we know we have the right codec, open the output file
						CStdString file = CONFIG.m_audioOutputPath + "/" + audioTapeRef->GetPath() + audioTapeRef->GetIdentifier();
						outFileRef->Open(file, AudioFile::WRITE, false, fileRef->GetSampleRate());
					}
					if(voIpSession)
					{	
						if(details.m_channel == 2)
						{
							decoder2->AudioChunkIn(chunkRef);
							decoder2->AudioChunkOut(tmpChunkRef);
						}
						else
						{
							decoder1->AudioChunkIn(chunkRef);
							decoder1->AudioChunkOut(tmpChunkRef);
						}
						filter->AudioChunkIn(tmpChunkRef);
						filter->AudioChunkOut(tmpChunkRef);
					}
					outFileRef->WriteChunk(tmpChunkRef);

					if(CONFIG.m_batchProcessingEnhancePriority == false)
					{
						// Give up CPU between every audio buffer to make sure the actual recording always has priority
						//ACE_Time_Value yield;
						//yield.set(0,1);	// 1 us
						//ACE_OS::sleep(yield);

						// Use this instead, even if it still seems this holds the whole process under Linux instead of this thread only.
						struct timespec ts;
						ts.tv_sec = 0;
						ts.tv_nsec = 1;
						ACE_OS::nanosleep (&ts, NULL);
					}
				}

				fileRef->Close();
				outFileRef->Close();

				if(CONFIG.m_deleteNativeFile)
				{
					fileRef->Delete();
					LOG4CXX_INFO(LOG.batchProcessingLog, CStdString("Th") + threadIdString + " deleting native: " + audioTapeRef->GetIdentifier());
				}

				// Finished processing the tape, pass on to next processor
				pBatchProcessing->RunNextProcessor(audioTapeRef);
			}
		}
		catch (CStdString& e)
		{
			LOG4CXX_ERROR(LOG.batchProcessingLog, CStdString("Th") + threadIdString + " " + e);
			if(fileRef.get()) {fileRef->Close();}
			if(outFileRef.get()) {outFileRef->Close();}
			if(CONFIG.m_deleteFailedCaptureFile && fileRef.get() != NULL)
			{
				LOG4CXX_INFO(LOG.batchProcessingLog, CStdString("Th") + threadIdString + " deleting native and transcoded");
				if(fileRef.get()) {fileRef->Delete();}
				if(outFileRef.get()) {outFileRef->Delete();}
			}
		}
		//catch(...)
		//{
		//	LOG4CXX_ERROR(LOG.batchProcessingLog, CStdString("unknown exception"));
		//}
	}
	LOG4CXX_INFO(LOG.batchProcessingLog, CStdString("Exiting thread Th" + threadIdString));
}


