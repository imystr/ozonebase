#include "../base/oz.h"
#include "ozVideoRecorder.h"

#include "../base/ozMotionFrame.h"
#include "../base/ozNotifyFrame.h"
#include "../libgen/libgenTime.h"


/**
* @brief 
*
* @return 
*/
int VideoRecorder::run()
{
    if ( waitForProviders() )
    {
        setReady();

        FrameRate inputFrameRate = videoProvider()->frameRate();
        Debug(1, "Provider framerate = %d/%d", inputFrameRate.num, inputFrameRate.den );

        //initEncoder();
        while( !mStop )
        {
            mQueueMutex.lock();
            if ( !mFrameQueue.empty() )
            {
                for ( FrameQueue::iterator iter = mFrameQueue.begin(); iter != mFrameQueue.end(); iter++ )
                {
                    processFrame( *iter );
                }
                mFrameQueue.clear();
            }
            mQueueMutex.unlock();
            checkProviders();
            usleep( INTERFRAME_TIMEOUT );
        }
        //deinitEncoder();
    }
    FeedProvider::cleanup();
    FeedConsumer::cleanup();
    return( 0 );
}

/**
* @brief 
*
* @param frame
*
* @return 
*/
bool VideoRecorder::processFrame( const FramePtr &frame )
{
    const AlarmFrame *alarmFrame = dynamic_cast<const AlarmFrame *>(frame.get());
    //const VideoProvider *provider = dynamic_cast<const VideoProvider *>(frame->provider());
    static uint64_t mLastAlarmTime;

    if ( !alarmFrame )
        return( false );

    AlarmState lastState = mState;
    
   

    if ( alarmFrame->alarmed() )
    {
     
        
        mState = ALARM;
        mLastAlarmTime = time64();
        
        
        if ( lastState == IDLE )
        {
            // Create new event
            mAlarmTime = mLastAlarmTime;
            mEventCount++;
            EventNotification::EventDetail detail( mEventCount, EventNotification::EventDetail::BEGIN );
            EventNotification *notification = new EventNotification( this, alarmFrame->id(), detail );
            distributeFrame( FramePtr( notification ) );
            std::string filename = stringtf( "%s/%s-%d.%s", mLocation.c_str(), mName.c_str(), mEventCount, mFormat.c_str() );
            initEncoder();
            openVideoFile( filename );
            for ( FrameStore::const_iterator iter = mFrameStore.begin(); iter != mFrameStore.end(); iter++ )
            {
                const AlarmFrame *frame = dynamic_cast<const AlarmFrame *>( iter->get() );
                if ( !frame )
                {
                    Error( "Unexpected frame type in frame store" );
                    continue;
                }
                encodeFrame( frame );
            }
        }
    }
    else if ( lastState == ALARM )
    {
        mState = ALERT;
    }

    if ( mState == ALERT )
    {
        
        if ( frame->age( mLastAlarmTime ) < -MAX_EVENT_TAIL_AGE )
        {
           
            if ((((double)mLastAlarmTime-mAlarmTime)/1000000.0) >= mMinTime)
            {
                closeVideoFile();
                deinitEncoder();
                mState = IDLE;
                EventNotification::EventDetail detail( mEventCount, ((double)mLastAlarmTime-mAlarmTime)/1000000.0 );
                EventNotification *notification = new EventNotification( this, alarmFrame->id(), detail );
                distributeFrame( FramePtr( notification ) );
                
            }
           
            
        }
        
    }

    if ( mState > IDLE )
    {
        encodeFrame( alarmFrame );
    }

    // Clear out old frames
    Debug( 5, "Got %lu frames in store", mFrameStore.size() );
    while( !mFrameStore.empty() )
    {
        FramePtr tempFrame = *(mFrameStore.begin());
        Debug( 5, "Frame %ju age %.2lf", tempFrame->id(), tempFrame->age() );
        if ( tempFrame->age() <= MAX_EVENT_HEAD_AGE )
            break;
        Debug( 5, "Deleting" );
        //delete tempFrame;
        mFrameStore.pop_front();
    }
    mFrameStore.push_back( frame );
    return( true );
}

void VideoRecorder::initEncoder()
{
    /* auto detect the output format from the name. default is mpeg. */
    AVOutputFormat *outputFormat = av_guess_format( mFormat.c_str(), NULL, NULL );
    if ( !outputFormat )
        Fatal( "Could not deduce output format from '%s'", mFormat.c_str() );

    // Force H.264
    //outputFormat->video_codec = AV_CODEC_ID_H264;

    /* allocate the output media context */
    mOutputContext = avformat_alloc_context();
    if ( !mOutputContext )
        Fatal( "Unable to allocate output context" );
    mOutputContext->oformat = outputFormat;

    mEncodeOpts = NULL;
    av_dict_set( &mOutputContext->metadata, "author", "Ozone Networks", 0 );
    av_dict_set( &mOutputContext->metadata, "comment", "Generated by Ozone Networks event recorder", 0 );

    // x264 Baseline
    avSetH264Preset( &mEncodeOpts, "medium" );
    av_dict_set( &mEncodeOpts, "b", "200k", 0 );
    av_dict_set( &mEncodeOpts, "bt", "240k", 0 );

    avDumpDict( mEncodeOpts );

    /* add the audio and video streams using the default format codecs and initialize the codecs */
    mVideoStream = NULL;
    AVCodecContext *videoCodecContext = NULL;
    if ( provider()->hasVideo() && (outputFormat->video_codec != CODEC_ID_NONE) )
    {
        //mVideoStream = av_new_stream( mOutputContext, 0 );
        mVideoStream = avformat_new_stream( mOutputContext, NULL );
        if ( !mVideoStream )
            Fatal( "Could not alloc video stream" );

        videoCodecContext = mVideoStream->codec;
        videoCodecContext->codec_id = outputFormat->video_codec;
        videoCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;

        videoCodecContext->width = mVideoParms.width();
        videoCodecContext->height = mVideoParms.height();
        //videoCodecContext->time_base.num = 1;
        //videoCodecContext->time_base.den = 15;
        //videoCodecContext->time_base = videoProvider()->frameRate().timeBase();
        videoCodecContext->time_base = mVideoParms.frameRate().timeBase();
        videoCodecContext->pix_fmt = mVideoParms.pixelFormat();
#if 0
        /* put sample parameters */
        videoCodecContext->bit_rate = mVideoParms.bitRate();
        /* resolution must be a multiple of two */
        videoCodecContext->width = mVideoParms.width();
        videoCodecContext->height = mVideoParms.height();
        /* time base: this is the fundamental unit of time (in seconds) in terms
           of which frame timestamps are represented. for fixed-fps content,
           timebase should be 1/framerate and timestamp increments should be
           identically 1. */
        videoCodecContext->time_base = mVideoParms.frameRate();
        videoCodecContext->gop_size = 12; /* emit one intra frame every twelve frames at most */
        videoCodecContext->pix_fmt = mVideoParms.pixelFormat();
#endif

        // some formats want stream headers to be separate
        if ( mOutputContext->oformat->flags & AVFMT_GLOBALHEADER )
            videoCodecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    uint16_t inputWidth = videoProvider()->width();
    uint16_t inputHeight = videoProvider()->height();
    PixelFormat inputPixelFormat = videoProvider()->pixelFormat();

    mConvertContext = NULL;
    if ( inputWidth != mVideoParms.width() || inputHeight != mVideoParms.height() )
    {
        // Prepare for image format and size conversions
        mConvertContext = sws_getContext( inputWidth, inputHeight, inputPixelFormat, mVideoParms.width(), mVideoParms.height(), mVideoParms.pixelFormat(), SWS_BICUBIC, NULL, NULL, NULL );
        if ( !mConvertContext )
            Fatal( "Unable to create conversion context for encoder" );
    }

    mAudioStream = NULL;
    AVCodecContext *audioCodecContext = NULL;
    if ( provider()->hasAudio() && (outputFormat->audio_codec != CODEC_ID_NONE) )
    {
        //mAudioStream = av_new_stream( mOutputContext, 1 );
        mAudioStream = avformat_new_stream( mOutputContext, NULL );
        if ( !mAudioStream )
            Fatal( "Could not alloc audio stream" );

        audioCodecContext = mAudioStream->codec;
        audioCodecContext->codec_id = outputFormat->audio_codec;

        audioCodecContext->codec_type = AVMEDIA_TYPE_AUDIO;

        /* put sample parameters */
        //audioCodecContext->bit_rate = mAudioParms.bitRate();
        //audioCodecContext->sample_rate = mAudioParms.sampleRate();
        //audioCodecContext->channels = mAudioParms.channels();
    }

#if 0
    /* set the output parameters (must be done even if no
       parameters). */
    if ( av_set_parameters( mOutputContext, NULL ) < 0 )
        Fatal( "Invalid output format parameters" );
#endif

    //av_dump_format( mOutputContext, 0, filename, 1 );

    /* now that all the parameters are set, we can open the audio and
       video codecs and allocate the necessary encode buffers */
    mAvInputFrame = NULL;
    mAvInterFrame = NULL;
    if ( mVideoStream )
    {
        /* find the video encoder */
        AVCodec *codec = avcodec_find_encoder( videoCodecContext->codec_id );
        if ( !codec )
            Fatal( "Video codec not found" );

        /* open the codec */
        if ( avcodec_open2( videoCodecContext, codec, &mEncodeOpts ) < 0 )
            Fatal( "Could not open video codec" );
        avDumpDict( mEncodeOpts );

        mAvInputFrame = av_frame_alloc();

        if ( !(mOutputContext->oformat->flags & AVFMT_RAWPICTURE) )
        {
            mVideoBuffer.size( 256000 ); // Guess?
        }
        if ( mConvertContext )
        {
            // Make space for anything that is going to be output
            mAvInterFrame = av_frame_alloc();
            mInterBuffer.size( avpicture_get_size( mVideoParms.pixelFormat(), mVideoParms.width(), mVideoParms.height() ) );
            avpicture_fill( (AVPicture *)mAvInterFrame, mInterBuffer.data(), mVideoParms.pixelFormat(), mVideoParms.width(), mVideoParms.height() );
        }
    }
    if ( mAudioStream )
    {
        /* find the audio encoder */
        AVCodec *codec = avcodec_find_encoder( audioCodecContext->codec_id );
        if ( !codec )
            Fatal( "Audio codec not found" );

        /* open it */
        if ( avcodec_open2( audioCodecContext, codec, &mEncodeOpts ) < 0 )
            Fatal( "Could not open audio codec" );
        avDumpDict( mEncodeOpts );

        mAudioBuffer.size( 8192 );
    }
}

void VideoRecorder::deinitEncoder()
{
    /* free the streams */
    for ( unsigned int i = 0; i < mOutputContext->nb_streams; i++ )
    {
        if ( mOutputContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            av_free( mAvInputFrame );
            av_free( mAvInterFrame );
        }
        else if ( mOutputContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO )
        {
        }
        /* close each codec */
        avcodec_close( mOutputContext->streams[i]->codec );
        av_freep( &mOutputContext->streams[i]->codec );
        av_freep( &mOutputContext->streams[i] );
    }

    av_dict_free( &mEncodeOpts );

    /* free the stream */
    av_free( mOutputContext );

    mOutputContext = NULL;
}

void VideoRecorder::openVideoFile( const std::string &filename )
{
    snprintf( mOutputContext->filename, sizeof(mOutputContext->filename), "%s", filename.c_str() );
    Info( "Opening video file '%s'", mOutputContext->filename );

    /* open the output file, if needed */
    if ( !(mOutputContext->oformat->flags & AVFMT_NOFILE) )
    {
        if ( avio_open( &mOutputContext->pb, filename.c_str(), AVIO_FLAG_WRITE ) < 0 )
        {
            Fatal( "Could not open output filename '%s'", filename.c_str() );
        }
        DiskIONotification::DiskIODetail detail( DiskIONotification::DiskIODetail::WRITE, DiskIONotification::DiskIODetail::BEGIN, filename );
        DiskIONotification *notification = new DiskIONotification( this, ++mNotificationId, detail );
        distributeFrame( FramePtr( notification ) );
    }

    av_dict_set( &mOutputContext->metadata, "title", stringtf( "Event Video %d", mEventCount ).c_str(), 0 );

    /* write the stream header, if any */
    avformat_write_header( mOutputContext, &mEncodeOpts );
    avDumpDict( mEncodeOpts );

    mVideoFrameCount = 0;
    mAudioFrameCount = 0;
}

void VideoRecorder::closeVideoFile()
{
    Info( "Closing video file '%s'", mOutputContext->filename );

    /* write the trailer, if any.  the trailer must be written
     * before you close the CodecContexts open when you wrote the
     * header; otherwise write_trailer may try to use memory that
     * was freed on av_codec_close() */
    av_write_trailer( mOutputContext );

    if ( !(mOutputContext->oformat->flags & AVFMT_NOFILE) )
    {
        size_t fileSize = avio_size( mOutputContext->pb );

        /* close the output file */
        avio_close( mOutputContext->pb );

        DiskIONotification::DiskIODetail detail( mOutputContext->filename, fileSize );
        DiskIONotification *notification = new DiskIONotification( this, ++mNotificationId, detail );
        distributeFrame( FramePtr( notification ) );
    }
}

void VideoRecorder::encodeFrame( const VideoFrame *frame )
{
    /* write interleaved audio and video frames */
    //if ( !mVideoStream || (mVideoStream && mAudioStream && audioTimeOffset < videoTimeOffset) )
    if ( frame->mediaType() == FeedFrame::FRAME_TYPE_AUDIO )
    {
        AVCodecContext *audioCodecContext = mAudioStream->codec;
        const AudioFrame *audioFrame = dynamic_cast<const AudioFrame *>(frame);

        AVPacket pkt;
        av_init_packet( &pkt );

        pkt.size = avcodec_encode_audio( audioCodecContext, mAudioBuffer.data(), mAudioBuffer.size(), (const int16_t *)audioFrame->buffer().data() );

        if ( audioCodecContext->coded_frame->pts != AV_NOPTS_VALUE )
            pkt.pts = av_rescale_q( audioCodecContext->coded_frame->pts, audioCodecContext->time_base, mAudioStream->time_base );
        pkt.flags |= AV_PKT_FLAG_KEY;
        pkt.stream_index = mAudioStream->index;
        pkt.data = mAudioBuffer.data();

        /* write the compressed frame in the media file */
        if ( av_interleaved_write_frame( mOutputContext, &pkt ) != 0)
            Fatal( "Error writing audio frame" );
        mAudioFrameCount++;
    }
    else
    {
        AVCodecContext *videoCodecContext = mVideoStream->codec;

        const FrameRate &inputFrameRate = videoProvider()->frameRate();
        Debug( 5, "PTS: %jd, %jd", videoCodecContext->coded_frame->pts, mVideoStream->pts.val );

        const VideoFrame *videoFrame = dynamic_cast<const VideoFrame *>(frame);
        Debug( 5, "PF:%d @ %dx%d", videoFrame->pixelFormat(), videoFrame->width(), videoFrame->height() );

        uint16_t inputWidth = videoProvider()->width();
        uint16_t inputHeight = videoProvider()->height();
        PixelFormat inputPixelFormat = videoProvider()->pixelFormat();
        avpicture_fill( (AVPicture *)mAvInputFrame, videoFrame->buffer().data(), inputPixelFormat, inputWidth, inputHeight );
        // XXX - Hack???
        mAvInputFrame->pts = (mVideoFrameCount * inputFrameRate.den * videoCodecContext->time_base.den) / (inputFrameRate.num * videoCodecContext->time_base.num);

        AVFrame *avOutputFrame = mAvInputFrame;
#if 0
        if ( mVideoFrameCount >= STREAM_NB_FRAMES )
        {
            /* no more frame to compress. The codec has a latency of a few
               frames if using B frames, so we get the last frames by
               passing the same picture again */
        }
        else
#endif
        {
            if ( mConvertContext )
            {
                if ( sws_scale( mConvertContext, mAvInputFrame->data, mAvInputFrame->linesize, 0, inputHeight, mAvInterFrame->data, mAvInterFrame->linesize ) < 0 )
                    Fatal( "Unable to convert input frame (%d@%dx%d) to output frame (%d@%dx%d) at frame %ju", inputPixelFormat, inputWidth, inputHeight, videoCodecContext->pix_fmt, videoCodecContext->width, videoCodecContext->height, mVideoFrameCount );
                avOutputFrame = mAvInterFrame;
                mAvInterFrame->pts = mAvInputFrame->pts;
            }
        }
        int result = 0;
        if ( mOutputContext->oformat->flags & AVFMT_RAWPICTURE )
        {
            Debug( 5, "Raw frame" );
            /* raw video case. The API will change slightly in the near futur for that */
            AVPacket pkt;
            av_init_packet(&pkt);

            pkt.flags |= AV_PKT_FLAG_KEY;
            pkt.stream_index = mVideoStream->index;
            pkt.data = (uint8_t *)avOutputFrame;
            pkt.size = sizeof(AVPicture);

            result = av_interleaved_write_frame( mOutputContext, &pkt );
        }
        else
        {
            Debug( 5, "Encoded frame" );
            int outSize = 0;
            do
            {
                /* encode the image */
                outSize = avcodec_encode_video( videoCodecContext, mVideoBuffer.data(), mVideoBuffer.capacity(), avOutputFrame );
                Debug( 5, "Outsize: %d", outSize );
                if ( outSize < 0 )
                {
                    Fatal( "Encoding failed" );
                }
                else if ( outSize > 0 )
                {
                    AVPacket pkt;
                    av_init_packet(&pkt);

                    if ( videoCodecContext->coded_frame->pts != AV_NOPTS_VALUE )
                        pkt.pts = av_rescale_q( videoCodecContext->coded_frame->pts, videoCodecContext->time_base, mVideoStream->time_base );
                    if ( videoCodecContext->coded_frame->key_frame )
                        pkt.flags |= AV_PKT_FLAG_KEY;
                    pkt.stream_index = mVideoStream->index;
                    pkt.data = mVideoBuffer.data();
                    pkt.size = outSize;

                    /* write the compressed frame in the media file */
                    result = av_interleaved_write_frame( mOutputContext, &pkt );
                }
                else
                {
                    /* if zero size, it means the image was buffered */
                }
                avOutputFrame = NULL;
            } while ( outSize > 0 && result == 0 );
        }
        if ( result != 0 )
            Fatal( "Error while writing video frame: %d", result );
        mVideoFrameCount++;
    }
}
