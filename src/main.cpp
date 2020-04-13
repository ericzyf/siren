extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <fmt/format.h>
#include <rtaudio/RtAudio.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cinttypes>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <vector>

struct RtAudioCbCtx {
    AVCodecContext *codecCtx;
    AVFormatContext *fmtCtx;
    AVPacket *packet;
    AVFrame *frame;
    std::queue<std::vector<uint8_t>> *frameQueue;
    int audioStreamIndex;
    int numChannels;
};

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

int RtAudioCb(void *outputBuffer,
         void *inputBuffer,
         unsigned int nFrames,
         double streamTime,
         RtAudioStreamStatus status,
         void *userData)
{
    auto ctx = (RtAudioCbCtx*)userData;

    if (status) {
        spdlog::get("stdout")->info("Stream underflow detected");
    }

    auto buffer = (uint8_t*)(outputBuffer);

    while (ctx->frameQueue->size() < nFrames) {
        if (av_read_frame(ctx->fmtCtx, ctx->packet) >= 0) {
            if (ctx->packet->stream_index == ctx->audioStreamIndex) {
                spdlog::get("stdout")->info("AVPacket->pts: {}", ctx->packet->pts);
                std::vector<std::vector<uint8_t>> packetData;
                if (decodePacket(ctx->packet, ctx->codecCtx, ctx->frame, packetData) < 0) {
                    // error occurred
                    return 1;
                }

                const bool isPlanarAudio = av_sample_fmt_is_planar(ctx->codecCtx->sample_fmt);
                const int bytesPerSample = av_get_bytes_per_sample(ctx->codecCtx->sample_fmt);

                if (isPlanarAudio) {
                    for (unsigned offset = 0; offset < packetData[0].size(); offset += bytesPerSample) {
                        std::vector<uint8_t> currSample(ctx->numChannels * bytesPerSample);
                        auto pSample = currSample.data();

                        for (int ch = 0; ch < ctx->numChannels; ++ch) {
                            memcpy(pSample, packetData[ch].data() + offset, bytesPerSample);
                            pSample += bytesPerSample;
                        }

                        ctx->frameQueue->emplace(currSample);
                    }
                } else {
                    // data is already interleaved
                    auto step = ctx->numChannels * bytesPerSample;
                    for (unsigned offset = 0; offset < packetData[0].size(); offset += step) {
                        std::vector<uint8_t> currSample(step);
                        memcpy(currSample.data(), packetData[0].data() + offset, step);
                        ctx->frameQueue->emplace(currSample);
                    }
                }
            }
        } else {
            spdlog::get("stdout")->info("End of stream");
            return 1;
        }
    }

    // have enough audio frames in the frameQueue
    // write $nFrames frames to outputBuffer
    unsigned maxNumFrames = std::min(nFrames, (unsigned)(ctx->frameQueue->size()));
    for (unsigned i = 0; i < nFrames; ++i) {
        const auto &frame = ctx->frameQueue->front();
        memcpy(buffer, frame.data(), frame.size());
        buffer += frame.size();
        ctx->frameQueue->pop();
    }

    return 0;
}

bool getRtAudioFormat(const AVCodecParameters *codecParams, RtAudioFormat &fmt)
{
    switch (codecParams->format) {

    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        fmt = RTAUDIO_SINT16;
        return true;

    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        fmt = RTAUDIO_SINT32;
        return true;

    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        fmt = RTAUDIO_FLOAT32;
        return true;

    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
        fmt = RTAUDIO_FLOAT64;
        return true;

    default:
        return false;

    }
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

    // init RtAudio
    spdlog::get("stdout")->debug("Initializing RtAudio");

    RtAudio dac(RtAudio::LINUX_PULSE);
    if (dac.getDeviceCount() < 1) {
        spdlog::get("stderr")->error("No audio devices found");
        std::exit(EXIT_FAILURE);
    }

    RtAudio::StreamParameters streamParams;
    streamParams.deviceId = dac.getDefaultOutputDevice();
    streamParams.nChannels = codecParams->channels;

    RtAudioFormat audioFmt;
    if (!getRtAudioFormat(codecParams, audioFmt)) {
        spdlog::get("stderr")->error("RtAudio error: unsupported audio format");
        std::exit(EXIT_FAILURE);
    }

    unsigned sampleRate = codecParams->sample_rate;
    unsigned bufferFrames = 256;

    spdlog::get("stdout")->debug("nChannels: {}", streamParams.nChannels);
    spdlog::get("stdout")->debug("sampleRate: {}", sampleRate);
    spdlog::get("stdout")->debug("bufferFrames: {}", bufferFrames);

    std::queue<std::vector<uint8_t>> frameQueue;

    RtAudioCbCtx ctx = {
        codecCtx,
        fmtCtx,
        packet,
        frame,
        &frameQueue,
        audioStreamIndex,
        codecParams->channels
    };

    try {
        dac.openStream(&streamParams, nullptr, audioFmt, sampleRate, &bufferFrames, RtAudioCb, &ctx);
        dac.startStream();
    } catch (RtAudioError &e) {
        e.printMessage();
        std::exit(EXIT_FAILURE);
    }

    fmt::print("Playing ... press <Enter> to quit\n");
    char input;
    std::cin.get(input);

    try {
        dac.stopStream();
    } catch (RtAudioError &e) {
        e.printMessage();
    }

    if (dac.isStreamOpen()) {
        dac.closeStream();
    }

    spdlog::get("stdout")->debug("Releasing resources");
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return 0;
}
