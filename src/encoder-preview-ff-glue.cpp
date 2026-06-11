#include "encoder-preview-ff-glue.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <climits>
#include <string_view>

static AVCodecID NameToAVCodecID(const std::string_view &str)
{
	if (str == "hevc")
		return AV_CODEC_ID_HEVC;
	if (str == "av1")
		return AV_CODEC_ID_AV1;
	if (str == "h264")
		return AV_CODEC_ID_H264;

	return AV_CODEC_ID_FIRST_UNKNOWN;
}

// Copied mostly from media-playback/media.c
static video_format AVPixelFormatToOBSFormat(int f)
{
	switch (f) {
	case AV_PIX_FMT_NONE:
		return VIDEO_FORMAT_NONE;
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_GRAY8:
		return VIDEO_FORMAT_Y800;
	case AV_PIX_FMT_YUV444P12LE:
		return VIDEO_FORMAT_I412;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YVYU422:
		return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUVA420P:
		return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_YUVA422P:
		return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P:
		return VIDEO_FORMAT_YUVA;
#if LIBAVUTIL_BUILD >= AV_VERSION_INT(56, 31, 100)
	case AV_PIX_FMT_YUVA444P12LE:
		return VIDEO_FORMAT_YA2L;
#endif
	case AV_PIX_FMT_BGR0:
		return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	default:;
	}

	return VIDEO_FORMAT_NONE;
}

static video_colorspace
AVColorSpaceToOBSSpace(AVColorSpace s, AVColorTransferCharacteristic trc,
		       AVColorPrimaries color_primaries)
{
	switch (s) {
	case AVCOL_SPC_BT709:
		return (trc == AVCOL_TRC_IEC61966_2_1) ? VIDEO_CS_SRGB
						       : VIDEO_CS_709;
	case AVCOL_SPC_FCC:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
		return (trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG
						       : VIDEO_CS_2100_PQ;
	default:
		return (color_primaries == AVCOL_PRI_BT2020)
			       ? ((trc == AVCOL_TRC_ARIB_STD_B67)
					  ? VIDEO_CS_2100_HLG
					  : VIDEO_CS_2100_PQ)
			       : VIDEO_CS_DEFAULT;
	}
}

static video_range_type AVRangeToOBSRange(AVColorRange r)
{
	return r == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_DEFAULT;
}

static void log_av_error(const char *method, int ret)
{
	char err[512];
	av_strerror(ret, err, sizeof(err));
	blog(LOG_ERROR, "%s failed with: %s", method, err);
}

bool CreateCodecContext(AVCodecContext **ctx, obs_encoder_t *enc)
{
	AVCodecID codec_id = NameToAVCodecID(obs_encoder_get_codec(enc));
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec)
		return false;

	*ctx = avcodec_alloc_context3(codec);

	(*ctx)->width = obs_encoder_get_width(enc);
	(*ctx)->height = obs_encoder_get_height(enc);

	/* Give the decoder the encoder headers (SPS/PPS/sequence header) up
	 * front so it can decode from the first keyframe. */
	uint8_t *extra_data;
	size_t extra_size;
	if (obs_encoder_get_extra_data(enc, &extra_data, &extra_size) &&
	    extra_size) {
		(*ctx)->extradata = static_cast<uint8_t *>(av_mallocz(
			extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
		memcpy((*ctx)->extradata, extra_data, extra_size);
		(*ctx)->extradata_size = static_cast<int>(extra_size);
	}

	if (int ret = avcodec_open2(*ctx, codec, nullptr)) {
		log_av_error("avcodec_open2", ret);
		avcodec_free_context(ctx);
		return false;
	}

	return true;
}

bool SendExtraData(AVCodecContext *ctx, obs_encoder_t *enc)
{
	encoder_packet packet = {};

	if (!obs_encoder_get_extra_data(enc, &packet.data, &packet.size)) {
		return true;
	}

	AVPacket av_packet = {};
	av_packet.size = static_cast<int>(packet.size);
	av_packet.data = packet.data;

	return avcodec_send_packet(ctx, &av_packet) == 0;
}

bool SendPacket(AVCodecContext *ctx, const encoder_packet *pkt)
{
	AVPacket av_packet = {};
	av_packet.size = static_cast<int>(pkt->size);
	av_packet.pts = pkt->pts;
	av_packet.dts = pkt->dts;
	av_packet.data = pkt->data;

	if (int ret = avcodec_send_packet(ctx, &av_packet)) {
		log_av_error("avcodec_send_packet", ret);
		return false;
	}

	return true;
}

bool ReceiveFrame(AVCodecContext *ctx, AVFrame *frame)
{
	int ret = avcodec_receive_frame(ctx, frame);

	if (!ret)
		return true;

	// EAGAIN isn't a failure, just needs more data
	if (ret != AVERROR(EAGAIN)) {
		log_av_error("avcodec_receive_frame", ret);
		return false;
	}

	return false;
}

bool AVFrameToSourceFrame(obs_source_frame *dst, AVFrame *src,
			  AVRational time_base)
{
	memset(dst, 0, sizeof(obs_source_frame));

	video_format format = AVPixelFormatToOBSFormat(src->format);

	/* Passing a frame with VIDEO_FORMAT_NONE (or zero dimensions) to
	 * obs_source_output_video() results in a zero-byte allocation, which
	 * libobs (31+) treats as a fatal error and crashes on. */
	if (format == VIDEO_FORMAT_NONE || !src->width || !src->height) {
		static int last_logged_format = INT_MIN;
		if (last_logged_format != src->format) {
			last_logged_format = src->format;
			const char *name = av_get_pix_fmt_name(
				static_cast<AVPixelFormat>(src->format));
			blog(LOG_WARNING,
			     "[obs-roi-ui] Dropping decoded frame with unsupported pixel format: %s (%dx%d)",
			     name ? name : "unknown", src->width, src->height);
		}
		return false;
	}

	dst->width = src->width;
	dst->height = src->height;
	dst->timestamp = av_rescale_q(src->best_effort_timestamp, time_base,
				      {1, 1000000000});

	video_range_type range = AVRangeToOBSRange(src->color_range);
	video_colorspace space = AVColorSpaceToOBSSpace(
		src->colorspace, src->color_trc, src->color_primaries);

	// ToDo: HDR support (max_luminance)
	dst->format = format;
	dst->full_range = range == VIDEO_RANGE_FULL;

	video_format_get_parameters_for_format(space, range, format,
					       dst->color_matrix,
					       dst->color_range_min,
					       dst->color_range_max);

	switch (src->color_trc) {
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_GAMMA22:
	case AVCOL_TRC_GAMMA28:
	case AVCOL_TRC_SMPTE170M:
	case AVCOL_TRC_SMPTE240M:
	case AVCOL_TRC_IEC61966_2_1:
		dst->trc = VIDEO_TRC_SRGB;
		break;
	case AVCOL_TRC_SMPTE2084:
		dst->trc = VIDEO_TRC_PQ;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		dst->trc = VIDEO_TRC_HLG;
		break;
	default:
		dst->trc = VIDEO_TRC_DEFAULT;
	}

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		dst->data[i] = src->data[i];
		dst->linesize[i] = abs(src->linesize[i]);
	}

	return true;
}
