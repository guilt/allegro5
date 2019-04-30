/*
 * Allegro video backend for FFmpeg
 * by Karthik Kumar Viswanathan <karthikkumar@gmail.com>.
 */

#include <stdio.h>
#include <math.h>
#include "allegro5/allegro5.h"
#include "allegro5/allegro_audio.h"
#include "allegro5/allegro_video.h"
#include "allegro5/internal/aintern_vector.h"
#include "allegro5/internal/aintern_video.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

ALLEGRO_DEBUG_CHANNEL("video")

//This should change when we start adding 10-bit Support.

//In-order for that, we need to pass hints to video addon
// (or) copy-pasta from ALLEGRO_DISPLAY *format

static const int RGB_PIXEL_FORMAT = ALLEGRO_PIXEL_FORMAT_BGR_888;
static enum AVPixelFormat FF_RGB_PIXEL_FORMAT = AV_PIX_FMT_RGB24;

typedef struct
{
   ALLEGRO_BITMAP* frameBitmap;
   bool endOfStream;
   bool pad[7];
} al_data;

typedef struct
{
  al_data ad;

  AVFormatContext* formatCtx;
  AVCodecContext* videoCodecCtx;
  AVCodecContext* audioCodecCtx;
  AVCodec* videoCodec;
  AVCodec* audioCodec;
  AVFrame* frameNative;
  AVFrame* frameRGB;
  struct SwsContext* swScaleCtx;
  uint8_t* bufferRGB;
  int pad[3];

} ffmpeg_data;

/* Video interface. */

static bool ffmpeg_set_video_playing(ALLEGRO_VIDEO* video)
{
   ASSERT(video && video->data);
   ffmpeg_data* fd = (ffmpeg_data*)video->data;
   al_data* ad = &(fd->ad);

   if (ad->endOfStream) video->playing = false;

   return true;
}

static bool ffmpeg_close_video(ALLEGRO_VIDEO* video)
{
   ASSERT(video && video->data);
   ffmpeg_data* fd = (ffmpeg_data*)(video->data);
   al_data* ad = &(fd->ad);

   ad->endOfStream = true;
   ffmpeg_set_video_playing(video);

   if (ad->frameBitmap) al_destroy_bitmap(ad->frameBitmap); ad->frameBitmap = NULL;

   if (fd->bufferRGB) av_free(fd->bufferRGB); fd->bufferRGB = NULL;
   if (fd->frameRGB) av_frame_free(&(fd->frameRGB)); fd->frameRGB = NULL;
   if (fd->frameNative) av_frame_free(&(fd->frameNative)); fd->frameNative = NULL;

   if (fd->videoCodec) avcodec_close(fd->videoCodecCtx); fd->videoCodec = NULL;
   if (fd->audioCodec) avcodec_close(fd->audioCodecCtx); fd->audioCodec = NULL;

   if (fd->swScaleCtx) sws_freeContext(fd->swScaleCtx); fd->swScaleCtx = NULL;

   if (fd->formatCtx) avformat_close_input(&(fd->formatCtx)); fd->formatCtx = NULL;

   al_free(fd);
   video->data = NULL;

   ALLEGRO_INFO("Closed Video.\n");

   return true;
}

static bool ffmpeg_open_video(ALLEGRO_VIDEO *video)
{
   const char* filename;
   AVFormatContext* pFormatCtx = NULL;
   int videoStreamIndex = -1;
   int audioStreamIndex = -1;

   ASSERT(video);
   ASSERT(video->filename);

   filename = al_path_cstr(video->filename, ALLEGRO_NATIVE_PATH_SEP);
   ASSERT(filename && *filename);

   if (avformat_open_input(&pFormatCtx, filename, NULL, NULL) != 0)
   {
      ALLEGRO_ERROR("Failed to open '%s'.\n", filename);
      return false;
   }

   ASSERT(pFormatCtx);

   ffmpeg_data* fd = al_malloc(sizeof(ffmpeg_data));
   if (!fd)
   {
      ALLEGRO_ERROR("Unable to allocate ffmpeg_data.\n");
      return false;
   }
   memset(fd, 0, sizeof(ffmpeg_data));
   fd->formatCtx = pFormatCtx;
   video->data = fd;

   if (avformat_find_stream_info(pFormatCtx, NULL) < 0 || pFormatCtx->nb_streams <= 0)
   {
      ALLEGRO_ERROR("Unable to find Stream Information for '%s'.\n", filename);
      ffmpeg_close_video(video);
      return false;
   }

#ifdef ALLEGRO_DEBUG
  av_dump_format(pFormatCtx, 0, filename, 0);
#endif

   for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++)
   {
      if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
      {
         if(videoStreamIndex != -1)
            ALLEGRO_WARN("Multiple Video Streams in File: '%s'. Ignoring Stream: %d \n", filename, i);
         else
            videoStreamIndex = i;
      }
      else if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
      {
         if (audioStreamIndex != -1)
            ALLEGRO_WARN("Multiple Audio Streams in File: '%s'. Ignoring Stream: %d \n", filename, i);
         else
            audioStreamIndex = i;
      }
   }

   if (videoStreamIndex == -1)
   {
      ALLEGRO_ERROR("No Video Streams in File: '%s'\n", filename);
      ffmpeg_close_video(video);
      return false;
   }
   if (audioStreamIndex == -1)
   {
      ALLEGRO_ERROR("No Audio Streams in File: '%s'\n", filename);
      ffmpeg_close_video(video);
      return false;
   }

   AVCodecContext* videoCodecCtx = pFormatCtx->streams[videoStreamIndex]->codec;
   AVCodecContext* audioCodecCtx = pFormatCtx->streams[audioStreamIndex]->codec;
   ASSERT(videoCodecCtx);
   ASSERT(audioCodecCtx);

   fd->videoCodecCtx = videoCodecCtx;
   fd->audioCodecCtx = audioCodecCtx;

   AVCodec *videoCodec = avcodec_find_decoder(videoCodecCtx->codec_id);
   AVCodec *audioCodec = avcodec_find_decoder(audioCodecCtx->codec_id);
   if (!videoCodec)
   {
      ALLEGRO_ERROR("Unable to obtain Video Codec for File: '%s'\n", filename);
      ffmpeg_close_video(video);
      return false;
   }
   if (avcodec_open2(videoCodecCtx, videoCodec, NULL) < 0)
   {
      ALLEGRO_ERROR("Unable to initialize Video Codec Decoder for File: '%s'\n", filename);
      ffmpeg_close_video(video);
      return false;
   }
   fd->videoCodec = videoCodec;

   if (!audioCodec)
   {
      ALLEGRO_ERROR("Unable to obtain Audio Codec for File: '%s'\n", filename);
      ffmpeg_close_video(video);
      return false;
   }
   if (avcodec_open2(audioCodecCtx, audioCodec, NULL) < 0)
   {
      ALLEGRO_ERROR("Unable to initialize Audio Codec Decoder for File: '%s'\n", filename);
      ffmpeg_close_video(video);
      return false;
   }
   fd->audioCodec = audioCodec;

   int numBytes = avpicture_get_size(FF_RGB_PIXEL_FORMAT, videoCodecCtx->width, videoCodecCtx->height);
   struct SwsContext* swScaleCtx = sws_getContext
   (
      videoCodecCtx->width,
      videoCodecCtx->height,
      videoCodecCtx->pix_fmt,
      videoCodecCtx->width,
      videoCodecCtx->height,
      FF_RGB_PIXEL_FORMAT,
      SWS_BILINEAR,
      NULL,
      NULL,
      NULL
   );
   if (!swScaleCtx)
   {
      ALLEGRO_ERROR("Unable to allocate Software Scaling Context.\n");
      ffmpeg_close_video(video);
      return false;
   }
   fd->swScaleCtx = swScaleCtx;

   AVFrame* frameNative = av_frame_alloc();
   if (!frameNative)
   {
      ALLEGRO_ERROR("Unable to allocate Frames.\n");
      ffmpeg_close_video(video);
      return false;
   }
   fd->frameNative = frameNative;

   AVFrame* frameRGB = av_frame_alloc();
   if (!frameRGB)
   {
      ALLEGRO_ERROR("Unable to allocate Frames.\n");
      ffmpeg_close_video(video);
      return false;
   }
   fd->frameRGB = frameRGB;

   uint8_t *bufferRGB = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
   if (!bufferRGB)
   {
      ALLEGRO_ERROR("Unable to allocate buffer.\n");
      ffmpeg_close_video(video);
      return false;
   }
   fd->bufferRGB = bufferRGB;
   avpicture_fill((AVPicture*)frameRGB, bufferRGB, FF_RGB_PIXEL_FORMAT, videoCodecCtx->width, videoCodecCtx->height);

   ALLEGRO_BITMAP *frameBitmap = al_create_bitmap(videoCodecCtx->width, videoCodecCtx->height);
   if (!frameBitmap)
   {
      ALLEGRO_ERROR("Unable to allocate Frame Bitmap.\n");
      ffmpeg_close_video(video);
      return false;
   }
   fd->ad.frameBitmap = frameBitmap;

   fd->ad.endOfStream = false;
   video->position = 0.0;

   video->fps = (double)(fd->videoCodecCtx->framerate.num)/((double)(fd->videoCodecCtx->framerate.den)+0.00001);
   video->scaled_width = videoCodecCtx->width;
   video->scaled_height = videoCodecCtx->height;
   video->video_position = 0.0;
   video->es_inited = false;
   video->playing = false;

   video->audio_rate = fd->audioCodecCtx->sample_rate;
   video->audio_position = 0.0;

   ALLEGRO_INFO("Opened Video: '%s'.\n", filename);

   return true;
}

static bool ffmpeg_start_video(ALLEGRO_VIDEO *video)
{
   ASSERT(video && video->data);
   ffmpeg_data* fd = (ffmpeg_data*)video->data;
   al_data* ad = &(fd->ad);

   video->playing = true;

   return false;
}

static bool ffmpeg_seek_video(ALLEGRO_VIDEO *video, double seek_to)
{
   ASSERT(video && video->data);
   ffmpeg_data* fd = (ffmpeg_data*)video->data;
   al_data* ad =  &(fd->ad);

   //Unsupported for now.

   return false;
}

static bool ffmpeg_update_video_frame_to_bitmap(AVFrame *frameRGB, ALLEGRO_BITMAP *bitmap, int width, int height)
{

   ASSERT(frameRGB);
   ASSERT(bitmap);

   ALLEGRO_LOCKED_REGION* a_lock = al_lock_bitmap(bitmap, RGB_PIXEL_FORMAT, ALLEGRO_LOCK_WRITEONLY);

   if (!a_lock)
   {
      ALLEGRO_ERROR("Unable to lock Bitmap for Update Video Frame.");
      return false;
   }

   // Write pixel data
   unsigned char* out = (unsigned char*)a_lock->data;
   int sizeBytes = width * 3;
   ASSERT(sizeBytes <= a_lock->pitch);

   if (sizeBytes == a_lock->pitch)
   {
      unsigned char* srcLine = frameRGB->data[0];
      int totalBytes = sizeBytes * height;
      memcpy(out, srcLine, totalBytes);
   }
   else
   {
      unsigned char* srcLine = frameRGB->data[0];
      int linePitch = frameRGB->linesize[0];
      for (int y = 0; y < height; ++y)
      {
         memcpy(out, srcLine, sizeBytes);
         srcLine += linePitch;
         out += a_lock->pitch;
      }
   }

   al_unlock_bitmap(bitmap);

   return true;
}

static bool ffmpeg_update_video(ALLEGRO_VIDEO *video)
{
   ASSERT(video && video->data);
   ffmpeg_data* fd = (ffmpeg_data*)video->data;
   al_data* ad = &(fd->ad);

   AVPacket packet;
   int frameRead = av_read_frame(fd->formatCtx, &packet);

   if (frameRead < 0)
   {
      ALLEGRO_INFO("Unable to Read Packet. Possible End-Of-File.");
      ad->endOfStream = true;
      ffmpeg_set_video_playing(video);
      return false;
   }

   int frameFinished = 0;

   //Decode Video
   avcodec_decode_video2(fd->videoCodecCtx, fd->frameNative, &frameFinished, &packet);
   if (!frameFinished)
      return false;

   //Here we Go!
   ALLEGRO_INFO("Decoded Frame: %lld\n", fd->frameNative->pkt_pos);

   //First Software Scale
   sws_scale(fd->swScaleCtx,
      (uint8_t *const *const)fd->frameNative->data, fd->frameNative->linesize,
      0, fd->videoCodecCtx->height,
      (uint8_t *const *)fd->frameRGB->data, fd->frameRGB->linesize);

   //Then Blit to BITMAP
   if (ffmpeg_update_video_frame_to_bitmap(fd->frameRGB, ad->frameBitmap, fd->videoCodecCtx->width, fd->videoCodecCtx->height))
   {
      video->current_frame = ad->frameBitmap;
      return true;
   }
   else
   {
      video->current_frame = NULL;
   }

   return false;
}

/* Exposed Video Interface */

static ALLEGRO_VIDEO_INTERFACE ffmpeg_vtable =
{
   ffmpeg_open_video,
   ffmpeg_close_video,
   ffmpeg_start_video,
   ffmpeg_set_video_playing,
   ffmpeg_seek_video,
   ffmpeg_update_video
};

ALLEGRO_VIDEO_INTERFACE *_al_video_ffmpeg_vtable(void)
{
   return &ffmpeg_vtable;
}

/* vim: set sts=3 sw=3 et: */
