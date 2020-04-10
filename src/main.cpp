extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cinttypes>
#include <cstdlib>
#include <vector>

int decodePacket(AVPacket *packet,
                 AVCodecContext *codecCtx,
                 AVFrame *frame,
                 std::vector<std::vector<uint8_t>> &data)
{
    bool isPlanarAudio = av_sample_fmt_is_planar(codecCtx->sample_fmt);
    int bytesPerSample = av_get_bytes_per_sample(codecCtx->sample_fmt);
    spdlog::get("stdout")->debug("isPlanarAudio: {}", isPlanarAudio);
    spdlog::get("stdout")->debug("bytesPerSample: {}", bytesPerSample);

    int res = avcodec_send_packet(codecCtx, packet);

    if (res < 0) {
        spdlog::get("stderr")->error("Error submitting the packet to the decoder");
        return res;
    }

    while (res >= 0) {
        res = avcodec_receive_frame(codecCtx, frame);
        if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
            return 0;
        } else if (res < 0) {
            spdlog::get("stderr")->error("Error during decoding");
            return res;
        }

        int numLines = isPlanarAudio ? codecCtx->channels : 1;
        for (int i = 0; i < numLines; ++i) {
            data.emplace_back(frame->data[i], frame->data[i] + frame->linesize[0]);

#ifndef NDEBUG
            spdlog::get("stdout")->debug("AVFrame->data[{}]", i);
            for (int j = 0; j < frame->linesize[0]; ++j) {
                fmt::print("{:02x} ", frame->data[i][j]);
            }
            fmt::print("\n");
#endif

        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    auto spdlog_stdout = spdlog::stdout_color_st("stdout");
    auto spdlog_stderr = spdlog::stderr_color_st("stderr");

    spdlog_stdout->set_level(spdlog::level::debug);
    spdlog_stderr->set_level(spdlog::level::debug);

    if (argc < 2) {
        fmt::print("Usage:\n\n");
        fmt::print("  siren <path-to-audio>\n");
        return 0;
    }

    AVFormatContext *fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        spdlog::get("stderr")->error("Failed to allocate memory for format context");
        std::exit(EXIT_FAILURE);
    }

    if (avformat_open_input(&fmtCtx, argv[1], nullptr, nullptr)) {
        spdlog::get("stderr")->error("Could not open file \"{}\"", argv[1]);
        std::exit(EXIT_FAILURE);
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        spdlog::get("stderr")->error("Could not get stream info");
        std::exit(EXIT_FAILURE);
    }

    int audioStreamIndex = -1;
    for (int i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            break;
        }
    }

    if (audioStreamIndex == -1) {
        spdlog::get("stderr")->error("Could not find audio stream");
        std::exit(EXIT_FAILURE);
    }

    AVCodecParameters *codecParams = fmtCtx->streams[audioStreamIndex]->codecpar;
    AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        spdlog::get("stderr")->error("Could not find codec");
        std::exit(EXIT_FAILURE);
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        spdlog::get("stderr")->error("Failed to allocate memory for codec context");
        std::exit(EXIT_FAILURE);
    }

    if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
        spdlog::get("stderr")->error("Failed to copy codec params to codec context");
        std::exit(EXIT_FAILURE);
    }

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        spdlog::get("stderr")->error("Failed to open codec through avcodec_open2");
        std::exit(EXIT_FAILURE);
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        spdlog::get("stderr")->error("Failed to allocate memory for AVFrame");
        std::exit(EXIT_FAILURE);
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        spdlog::get("stderr")->error("Failed to allocate memory for AVPacket");
        std::exit(EXIT_FAILURE);
    }

    int res = 0;
    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index == audioStreamIndex) {
            spdlog::get("stdout")->info("AVPacket->pts: {}", packet->pts);
            std::vector<std::vector<uint8_t>> data;
            res = decodePacket(packet, codecCtx, frame, data);
            if (res < 0) {
                break;
            }
        }
        av_packet_unref(packet);
    }

    spdlog::get("stdout")->debug("Releasing resources");
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return 0;
}
