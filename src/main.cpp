extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cxxopts.hpp>
#include <fmt/format.h>
#include <portaudio.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cinttypes>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <vector>

struct PaStreamCbCtx {
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

    // init data
    data.resize(isPlanarAudio ? codecCtx->channels : 1);

    while (res >= 0) {
        res = avcodec_receive_frame(codecCtx, frame);
        if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
            return 0;
        } else if (res < 0) {
            spdlog::get("stderr")->error("Error during decoding");
            return res;
        }

        if (isPlanarAudio) {
            auto lineSize = frame->nb_samples * bytesPerSample;
            for (int i = 0; i < codecCtx->channels; ++i) {
                data[i].insert(data[i].end(), frame->data[i], frame->data[i] + lineSize);
            }
        } else {
            auto lineSize = frame->nb_samples * bytesPerSample * codecCtx->channels;
            data[0].insert(data[0].end(), frame->data[0], frame->data[0] + lineSize);
        }

#ifndef NDEBUG
        for (const auto &row : data) {
            for (const auto &sample : row) {
                fmt::print("{:02x} ", sample);
            }
            fmt::print("\n");
        }
#endif

    }

    return 0;
}

int pa_stream_cb(const void *inputBuffer,
                 void *outputBuffer,
                 unsigned long frameCount,
                 const PaStreamCallbackTimeInfo *timeInfo,
                 PaStreamCallbackFlags statusFlags,
                 void *userData)
{
    auto ctx = (PaStreamCbCtx*)userData;
    auto output = (uint8_t*)outputBuffer;

    while (ctx->frameQueue->size() < frameCount) {
        if (av_read_frame(ctx->fmtCtx, ctx->packet) >= 0) {
            if (ctx->packet->stream_index == ctx->audioStreamIndex) {
                spdlog::get("stdout")->info("AVPacket->pts: {}", ctx->packet->pts);
                std::vector<std::vector<uint8_t>> packetData;
                int ret = decodePacket(ctx->packet, ctx->codecCtx, ctx->frame, packetData);
                if (ret < 0) {
                    // error occurred
                    if (ret != AVERROR(EAGAIN) &&
                        ret != AVERROR_EOF &&
                        ret != AVERROR(EINVAL) &&
                        ret != AVERROR(ENOMEM)) {
                        // cannot decode this packet, skip it
                        av_packet_unref(ctx->packet);
                        continue;
                    } else {
                        return paComplete;
                    }
                }

                const bool isPlanarAudio = av_sample_fmt_is_planar(ctx->codecCtx->sample_fmt);
                const int bytesPerSample = av_get_bytes_per_sample(ctx->codecCtx->sample_fmt);

                if (isPlanarAudio) {
                    std::vector<uint8_t> currSample(ctx->numChannels * bytesPerSample);
                    for (unsigned offset = 0; offset < packetData[0].size(); offset += bytesPerSample) {
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
                    std::vector<uint8_t> currSample(step);
                    for (unsigned offset = 0; offset < packetData[0].size(); offset += step) {
                        memcpy(currSample.data(), packetData[0].data() + offset, step);
                        ctx->frameQueue->emplace(currSample);
                    }
                }
            }
            av_packet_unref(ctx->packet);
        } else {
            if (!ctx->frameQueue->empty()) {
                // end of stream
                // write remaining audio frames in frameQueue to outputBuffer
                auto numRemFrames = ctx->frameQueue->size();
                for (unsigned i = 0; i < numRemFrames; ++i) {
                    const auto &frame = ctx->frameQueue->front();
                    memcpy(output, frame.data(), frame.size());
                    output += frame.size();
                    ctx->frameQueue->pop();
                }
            }
            spdlog::get("stdout")->info("End of stream");
            return paComplete;
        }
    }

    // have enough audio frames in the frameQueue
    // write $nFrames frames to outputBuffer
    auto maxNumFrames = std::min(frameCount, (decltype(frameCount))(ctx->frameQueue->size()));
    for (decltype(maxNumFrames) i = 0; i < frameCount; ++i) {
        const auto &frame = ctx->frameQueue->front();
        memcpy(output, frame.data(), frame.size());
        output += frame.size();
        ctx->frameQueue->pop();
    }

    return paContinue;
}

bool getPaSampleFormat(const AVCodecParameters *codecParams, PaSampleFormat &fmt)
{
    switch (codecParams->format) {

    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        fmt = paUInt8;
        return true;

    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        fmt = paInt16;
        return true;

    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        fmt = paInt32;
        return true;

    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        fmt = paFloat32;
        return true;

    default:
        return false;

    }

    return false;
}

void handlePaError(const PaError &err)
{
    Pa_Terminate();
    spdlog::get("stderr")->error("An error occurred while using the PortAudio stream");
    spdlog::get("stderr")->error("Error number: {}", err);
    spdlog::get("stderr")->error("Error message: {}", Pa_GetErrorText(err));
    std::exit(EXIT_FAILURE);
}

void listHostApiInfo()
{
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        handlePaError(err);
    }

    int apiCount = Pa_GetHostApiCount();
    fmt::print("HostApiCount: {}\n", apiCount);
    for (int i = 0; i < apiCount; ++i) {
        auto info = Pa_GetHostApiInfo(i);
        fmt::print("[{}] {}\n", i, info->name);
    }

    Pa_Terminate();
}

int main(int argc, char *argv[])
{
    // parse args
    std::string arg_file;
    int arg_verbosity;
    int arg_samplerate;
    unsigned long arg_bufferframes;

    try {
        cxxopts::Options options("siren");
        options.add_options()
            ("h,help", "Show help")
            ("f,file", "Path to the audio file", cxxopts::value<std::string>())
            ("v,verbosity", "Set log verbosity(0~6)", cxxopts::value<int>()->default_value("2"))
            ("s,samplerate", "Set sample rate(0: auto)", cxxopts::value<int>()->default_value("0"))
            ("b,bufferframes", "Set number of buffer frames", cxxopts::value<unsigned long>()->default_value("512"))
            ("listhostapi", "List host audio API")
        ;

        if (argc == 1) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        auto parseResult = options.parse(argc, argv);

        if (parseResult.count("help")) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        if (parseResult.count("listhostapi")) {
            listHostApiInfo();
            return 0;
        }

        if (parseResult.count("file")) {
            arg_file = parseResult["file"].as<std::string>();
        } else {
            std::cerr << "Please specify the path to audio file" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        arg_verbosity = parseResult["verbosity"].as<int>();
        arg_samplerate = parseResult["samplerate"].as<int>();
        arg_bufferframes = parseResult["bufferframes"].as<unsigned long>();
    } catch (const cxxopts::OptionException &e) {
        std::cerr << "Error parsing args" << std::endl;
        std::cerr << e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
    // finish parsing args
    //////////////////////

    auto spdlog_stdout = spdlog::stdout_color_st("stdout");
    auto spdlog_stderr = spdlog::stderr_color_st("stderr");

    spdlog_stdout->set_level((spdlog::level::level_enum)arg_verbosity);
    spdlog_stderr->set_level((spdlog::level::level_enum)arg_verbosity);

    AVFormatContext *fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        spdlog::get("stderr")->error("Failed to allocate memory for format context");
        std::exit(EXIT_FAILURE);
    }

    if (avformat_open_input(&fmtCtx, arg_file.c_str(), nullptr, nullptr)) {
        spdlog::get("stderr")->error("Could not open file \"{}\"", arg_file.c_str());
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

    // init PortAudio
    spdlog::get("stdout")->debug("Initializing PortAudio");

    int sampleRate = (arg_samplerate == 0) ? codecParams->sample_rate : arg_samplerate;

    spdlog::get("stdout")->debug("nChannels: {}", codecCtx->channels);
    spdlog::get("stdout")->debug("sampleRate: {}", sampleRate);
    spdlog::get("stdout")->debug("bufferFrames: {}", arg_bufferframes);

    PaStream *stream = nullptr;
    PaError err;

    err = Pa_Initialize();
    if (err != paNoError) {
        handlePaError(err);
    }

    auto hostApiInfo = Pa_GetHostApiInfo(Pa_GetDefaultHostApi());
    spdlog::get("stdout")->info("Current audio API: {}", hostApiInfo->name);

    PaSampleFormat sampleFmt;
    if (!getPaSampleFormat(codecParams, sampleFmt)) {
        spdlog::get("stderr")->error("PortAudio error: unsupported sample format");
        std::exit(EXIT_FAILURE);
    }

    std::queue<std::vector<uint8_t>> frameQueue;

    PaStreamCbCtx pactx = {
        codecCtx,
        fmtCtx,
        packet,
        frame,
        &frameQueue,
        audioStreamIndex,
        codecCtx->channels
    };

    err = Pa_OpenDefaultStream(
        &stream,
        0,
        codecCtx->channels,
        sampleFmt,
        sampleRate,
        arg_bufferframes,
        pa_stream_cb,
        &pactx
    );
    if (err != paNoError) {
        handlePaError(err);
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        handlePaError(err);
    }

    fmt::print("Playing ... press <Enter> to quit\n");
    char input;
    std::cin.get(input);

    err = Pa_StopStream(stream);
    if (err != paNoError) {
        handlePaError(err);
    }

    err = Pa_CloseStream(stream);
    if (err != paNoError) {
        handlePaError(err);
    }

    Pa_Terminate();

    spdlog::get("stdout")->debug("Releasing resources");
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return 0;
}
