#pragma once

// ROS header file
#include <ros/ros.h>
#include <std_msgs/String.h>

#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/time.h>
#include <iostream>
#include <thread>
#include <future>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <atomic>
#include <memory> 

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <SDL.h>
}
using namespace std;

#define SDL_AUDIO_BUFFER_SIZE 2048
#define MAX_AUDIO_FRAME_SIZE 192000

bool is_shutdown = false;
void signal_handler(int sig)
{
    ROS_INFO("Received Ctrl+C signal. Shutting down...");
    is_shutdown = true;
}

typedef struct packet_queue_t
{
    AVPacketList *first_pkt;
    AVPacketList *last_pkt;
    int nb_packets;   // 队列中AVPacket的个数
    int size;         // 队列中AVPacket总的大小(字节数)
    SDL_mutex *mutex; // 互斥锁
    SDL_cond *cond;   // 条件变量
} packet_queue_t;

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} FF_AudioParams;

packet_queue_t s_audio_pkt_queue;
FF_AudioParams s_audio_param_src;
FF_AudioParams s_audio_param_tgt;

class ParseRtsp
{
  public:
    ParseRtsp():n("~"), is_rtsp_stream_coming(false)
    {
        //av_init_packet(p_packet);
        p_packet = (AVPacket *)av_malloc(sizeof(AVPacket));
        p_frame = av_frame_alloc();
        action_cmd_sub = n.subscribe("/cloud_command", 10, &ParseRtsp::msg_callback_func, this);
        media_state_pub = n.advertise<std_msgs::String>("/media_state", 1000);
        thread_ = std::thread(std::bind(&ParseRtsp::recv_rtsp_stream, this));
        ros::spin();
    }
    ~ParseRtsp()
    {

    }

    void packet_queue_init(packet_queue_t *q)
    {
        memset(q, 0, sizeof(packet_queue_t));
        q->mutex = SDL_CreateMutex();
        q->cond = SDL_CreateCond();
    }

    int packet_queue_push(packet_queue_t *q, AVPacket *pkt)
    {
        AVPacketList *pkt_list;
        
        if (av_packet_make_refcounted(pkt) < 0)
        {
            ROS_ERROR("[pkt] is not refrence counted\n");
            return -1;
        }
        pkt_list = (AVPacketList*)av_malloc(sizeof(AVPacketList));
        
        if (!pkt_list)
        {
            return -1;
        }
        
        pkt_list->pkt = *pkt;
        pkt_list->next = NULL;

        SDL_LockMutex(q->mutex);

        if (!q->last_pkt)   // 队列为空
        {
            q->first_pkt = pkt_list;
        }
        else
        {
            q->last_pkt->next = pkt_list;
        }
        q->last_pkt = pkt_list;
        q->nb_packets++;
        q->size += pkt_list->pkt.size;
        // 发个条件变量的信号：重启等待q->cond条件变量的一个线程
        SDL_CondSignal(q->cond);

        SDL_UnlockMutex(q->mutex);
        return 0;
    }

    int audio_decode_frame(AVCodecContext *codec_ctx, AVPacket *packet, uint8_t *audio_buf, int buf_size)
    {
        
        
        int frm_size = 0;
        int res = 0;
        int ret = 0;
        int nb_samples = 0;             // 重采样输出样本数
        uint8_t *p_cp_buf = NULL;
        int cp_len = 0;
        bool need_new = false;

        res = 0;
        while (1)
        {
            need_new = false;
            // 1 接收解码器输出的数据，每次接收一个frame
            ret = avcodec_receive_frame(codec_ctx, p_frame);
            if (ret != 0)
            {
                if (ret == AVERROR_EOF)
                {
                    ROS_ERROR("audio avcodec_receive_frame(): the decoder has been fully flushed\n");
                    res = 0;
                    av_frame_unref(p_frame);
                    //av_frame_free(&p_frame);
                    return res;
                }
                else if (ret == AVERROR(EAGAIN))
                {
                    need_new = true;
                }
                else if (ret == AVERROR(EINVAL))
                {
                    ROS_ERROR("audio avcodec_receive_frame(): codec not opened, or it is an encoder\n");
                    res = -1;
                    av_frame_unref(p_frame);
                    //av_frame_free(&p_frame);
                    return res;
                }
                else
                {
                    ROS_ERROR("audio avcodec_receive_frame(): legitimate decoding errors\n");
                    res = -1;
                    //av_frame_unref(p_frame);
                    av_frame_free(&p_frame);
                    return res;
                }
            }
            else
            {              
                if (!is_initial)
                {
                    //swr_free(&s_audio_swr_ctx);

                    s_audio_swr_ctx = swr_alloc_set_opts(s_audio_swr_ctx,
                                        av_get_default_channel_layout(s_audio_param_tgt.channels), 
                                        s_audio_param_tgt.fmt, 
                                        s_audio_param_tgt.freq,
                                        av_get_default_channel_layout(p_codec_par->channels),           
                                        (AVSampleFormat)p_codec_par->format, 
                                        p_codec_par->sample_rate,
                                        0,
                                        NULL);
                    
                    if (s_audio_swr_ctx == NULL || swr_init(s_audio_swr_ctx) < 0)
                    {
                        ROS_ERROR("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                                p_frame->sample_rate, av_get_sample_fmt_name((AVSampleFormat)p_frame->format), p_frame->channels,
                                s_audio_param_tgt.freq, av_get_sample_fmt_name(s_audio_param_tgt.fmt), s_audio_param_tgt.channels);
                        swr_free(&s_audio_swr_ctx);
                        return -1;
                    }
                
                    is_initial = true;
                }

                if (s_audio_swr_ctx != NULL)        // 重采样
                {
                    // 重采样输入参数1：输入音频样本数是p_frame->nb_samples
                    // 重采样输入参数2：输入音频缓冲区
                    const uint8_t **in = (const uint8_t **)p_frame->extended_data;
                    // 重采样输出参数1：输出音频缓冲区尺寸
                    // 重采样输出参数2：输出音频缓冲区
                    uint8_t **out = &s_resample_buf;
                    // 重采样输出参数：输出音频样本数(多加了256个样本)
                    int out_count = (int64_t)p_frame->nb_samples * s_audio_param_tgt.freq / p_frame->sample_rate + 256;
                    // 重采样输出参数：输出音频缓冲区尺寸(以字节为单位)
                    int out_size  = av_samples_get_buffer_size(NULL, s_audio_param_tgt.channels, out_count, s_audio_param_tgt.fmt, 0);
                    if (out_size < 0)
                    {
                        ROS_ERROR("av_samples_get_buffer_size() failed\n");
                        return -1;
                    }
                    
                    if (s_resample_buf == NULL)
                    {
                        av_fast_malloc(&s_resample_buf, &s_resample_buf_len, out_size);
                    }
                    if (s_resample_buf == NULL)
                    {
                        return AVERROR(ENOMEM);
                    }
                    // 音频重采样：返回值是重采样后得到的音频数据中单个声道的样本数
                    nb_samples = swr_convert(s_audio_swr_ctx, out, out_count, in, p_frame->nb_samples);
                    if (nb_samples < 0) {
                        ROS_ERROR("swr_convert() failed\n");
                        return -1;
                    }
                    if (nb_samples == out_count)
                    {
                        ROS_WARN("audio buffer is probably too small\n");
                        if (swr_init(s_audio_swr_ctx) < 0)
                            swr_free(&s_audio_swr_ctx);
                    }
            
                    // 重采样返回的一帧音频数据大小(以字节为单位)
                    p_cp_buf = s_resample_buf;
                    cp_len = nb_samples * s_audio_param_tgt.channels * av_get_bytes_per_sample(s_audio_param_tgt.fmt);
                }
                else 
                {
                    // 根据相应音频参数，获得所需缓冲区大小
                    frm_size = av_samples_get_buffer_size(
                            NULL, 
                            codec_ctx->channels,
                            p_frame->nb_samples,
                            codec_ctx->sample_fmt,
                            1);
                    
                    ROS_INFO("frame size %d, buffer size %d\n", frm_size, buf_size);
                    assert(frm_size <= buf_size);

                    p_cp_buf = p_frame->data[0];
                    cp_len = frm_size;
                }
                
                // 将音频帧拷贝到函数输出参数audio_buf
                memcpy(audio_buf, p_cp_buf, cp_len);

                res = cp_len;
                av_frame_unref(p_frame);
                //av_frame_free(&p_frame);
                return res;
            }

            // 2 向解码器喂数据，每次喂一个packet
            if (need_new)
            {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret != 0)
                {
                    ROS_ERROR("avcodec_send_packet() failed %d\n", ret);
                    av_packet_unref(packet);
                    res = -1;
                    //av_frame_unref(p_frame);
                    av_frame_free(&p_frame);
                    return res;
                }
            }
        }        
        
    }

    int packet_queue_pop(packet_queue_t *q, AVPacket *pkt, int block)
    {
        AVPacketList *p_pkt_node;
        int ret;

        SDL_LockMutex(q->mutex);
        while (1)
        {
            p_pkt_node = q->first_pkt;
            if (p_pkt_node)             // 队列非空，取一个出来
            {
                q->first_pkt = p_pkt_node->next;
                if (!q->first_pkt)
                {
                    q->last_pkt = NULL;
                }
                q->nb_packets--;
                q->size -= p_pkt_node->pkt.size;
                *pkt = p_pkt_node->pkt;
                //av_packet_ref(pkt, &p_pkt_node->pkt);
                av_free(p_pkt_node);
                ret = 1;
                break;
            }
            else if (is_input_finished)  // 队列已空，文件已处理完
            {
                ret = 0;
                break;
            }
            else if (!block)            // 队列空且阻塞标志无效，则立即退出
            {
                ret = 0;
                break;
            }
            else                        // 队列空且阻塞标志有效，则等待
            {
                SDL_CondWait(q->cond, q->mutex);
            }
        }
        SDL_UnlockMutex(q->mutex);
        return ret;
    }

    // 音频处理回调函数。读队列获取音频包，解码，播放
    // 此函数被SDL按需调用，此函数不在用户主线程中，因此数据需要保护
    // \param[in]  userdata用户在注册回调函数时指定的参数
    // \param[out] stream 音频数据缓冲区地址，将解码后的音频数据填入此缓冲区
    // \param[out] len    音频数据缓冲区大小，单位字节
    // 回调函数返回后，stream指向的音频缓冲区将变为无效
    // 双声道采样点的顺序为LRLRLR
    void static sdl_audio_callback(void *userdata, uint8_t *stream, int len)
    {
        ParseRtsp* rtsp = static_cast<ParseRtsp*>(userdata);

        //AVCodecContext *p_codec_ctx = (AVCodecContext *)userdata;
        int copy_len;           // 
        int get_size;           // 获取到解码后的音频数据大小

        static uint8_t s_audio_buf[(MAX_AUDIO_FRAME_SIZE*3)/2]; // 1.5倍声音帧的大小
        static uint32_t s_audio_len = 0;    // 新取得的音频数据大小
        static uint32_t s_tx_idx = 0;       // 已发送给设备的数据量

        AVPacket *packet  = (AVPacket *)av_malloc(sizeof(AVPacket));
        //std::unique_ptr<AVPacket, decltype(&av_packet_free)> packet(av_packet_alloc(), av_packet_free);

        int frm_size = 0;
        int ret_size = 0;
        int ret;
        while (len > 0)         // 确保stream缓冲区填满，填满后此函数返回
        {
            if (rtsp->is_decode_finished)  // 解码完成标志
            {
                if(packet)
                {
                    av_packet_free(&packet);
                    packet = NULL;
                }
                return;
            }
            if (s_tx_idx >= s_audio_len)
            {   // audio_buf缓冲区中数据已全部取出，则从队列中获取更多数据
                //packet = (AVPacket *)av_malloc(sizeof(AVPacket));
                // 1. 从队列中读出一包音频数据
                if (rtsp->packet_queue_pop(&s_audio_pkt_queue, packet, 1) <= 0)
                {
                    if (rtsp->is_input_finished)
                    {
                        av_packet_unref(packet);
                        //packet = NULL;    // flush decoder
                        ROS_INFO("Flushing decoder...\n");
                    }
                    else
                    {
                        //av_packet_unref(packet);
                        if(packet)
                        {
                            av_packet_free(&packet);
                            packet = NULL;
                        }
                        return;
                    }
                }
        
                // 2. 解码音频包
                get_size = rtsp->audio_decode_frame(rtsp->p_codec_ctx, packet, s_audio_buf, sizeof(s_audio_buf));
                if (get_size < 0)
                {
                    // 出错输出一段静音
                    s_audio_len = 1024; // arbitrary?
                    memset(s_audio_buf, 0, s_audio_len);
                    av_packet_unref(packet);
                }
                else if (get_size == 0) // 解码缓冲区被冲洗，整个解码过程完毕
                {
                    rtsp->is_decode_finished = true;
                }
                else
                {
                    s_audio_len = get_size;
                    av_packet_unref(packet);
                }
                s_tx_idx = 0;

                if (packet->data != NULL)
                {
                    //av_packet_unref(p_packet);
                }
            }

            copy_len = s_audio_len - s_tx_idx;
            if (copy_len > len)
            {
                copy_len = len;
            }

            // 将解码后的音频帧(s_audio_buf+)写入音频设备缓冲区(stream)，播放
            memcpy(stream, (uint8_t *)s_audio_buf + s_tx_idx, copy_len);
            len -= copy_len;
            stream += copy_len;
            s_tx_idx += copy_len;
        }

        if(packet)
        {
            av_packet_free(&packet);
            packet = NULL;
        }
    }

    void recv_rtsp_stream()
    {
        while(ros::ok() &&!is_shutdown)
        {
            if(is_rtsp_stream_coming == true)
            {
                SDL_PauseAudio(0);
                {
                    std_msgs::String msg;
                    msg.data = "begin to play";
                    media_state_pub.publish(msg);
                }
                
                // A4. 从视频文件中读取一个packet，此处仅处理音频packet
                //     对于音频来说，若是帧长固定的格式则一个packet可包含整数个frame，
                //     若是帧长可变的格式则一个packet只包含一个frame
                while (is_rtsp_stream_coming && av_read_frame(p_fmt_ctx, p_packet) == 0)
                {
                    if (p_packet->stream_index == a_idx)
                    {
                        packet_queue_push(&s_audio_pkt_queue, p_packet);
                    }
                    else
                    {
                        cout <<"jajjaja" <<endl;
                        av_packet_unref(p_packet);
                    }
                }
                
                if(is_rtsp_stream_coming == true) // 不是人为关闭，说明读取过程失败
                {
                    std_msgs::String msg;
                    msg.data = "failed to play";
                    media_state_pub.publish(msg);
                    is_rtsp_stream_coming = false;
                }
                
                SDL_PauseAudio(1); // 暂停音频设备
                SDL_Delay(40);
                SDL_Delay(1500);
                release();
            }
            else
            {
                while(is_rtsp_stream_coming == false )
                {
                    std::cout <<"进入休眠状态，等待被唤醒" <<endl;
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv.wait(lock);

                    if(is_shutdown)
                        return ;
                }
                if(!init_codec_sdl()) // 线程被唤醒后，进行初始化
                {
                    std_msgs::String msg;
                    msg.data = "intial failed";
                    media_state_pub.publish(msg);
                    ROS_ERROR("初始化失败，重新进入休眠状态");
                    is_rtsp_stream_coming = false;
                }
            }
        }
    }

    void msg_callback_func(const std_msgs::String &input)
    {
        Analysis(input.data);

        if(control_msg.size() < 2 || control_msg.size() > 3)
        {
            cout <<"Wrong Callback Msg" <<endl;
        }

        if(control_msg[0] == "start_media_pull")
        {
            is_rtsp_stream_coming = true;
            rtsp_url = control_msg[2];
            cv.notify_one(); 
        }
        else if(control_msg[0] == "stop_media_pull")
        {
            is_rtsp_stream_coming = false;
        }
        control_msg.clear();
    }
    
    bool init_codec_sdl()
    {
        s_audio_swr_ctx = swr_alloc();
    
        int ret = avformat_open_input(&p_fmt_ctx, rtsp_url.c_str(), NULL, NULL);
        if (ret != 0)
        {
            ROS_ERROR("avformat_open_input() failed %d\n", ret);
  
            if (s_resample_buf != NULL)
            {
                av_free(s_resample_buf);
            }
            return false;
        }

        // A1.2 搜索流信息：读取一段视频文件数据，尝试解码，将取到的流信息填入p_fmt_ctx->streams
        //      p_fmt_ctx->streams是一个指针数组，数组大小是pFormatCtx->nb_streams
        ret = avformat_find_stream_info(p_fmt_ctx, NULL);
        if (ret < 0)
        {
            ROS_ERROR("avformat_find_stream_info() failed %d\n", ret);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        // 将文件相关信息打印在标准错误设备上
        av_dump_format(p_fmt_ctx, 0, nullptr, 0);

        // A2. 查找第一个音频流
        a_idx = -1;
        for (int i=0; i < p_fmt_ctx->nb_streams; i++)
        {
            if (p_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                a_idx = i;
                ROS_INFO("Find a audio stream, index %d\n", a_idx);
                break;
            }
        }
        if (a_idx == -1)
        {
            ROS_ERROR("Cann't find audio stream\n");
            avformat_close_input(&p_fmt_ctx);
            return false;
        }

        // A3. 为音频流构建解码器AVCodecContext
        // A3.1 获取解码器参数AVCodecParameters
        p_codec_par = p_fmt_ctx->streams[a_idx]->codecpar;

        // A3.2 获取解码器
        p_codec = avcodec_find_decoder(p_codec_par->codec_id);
        if (p_codec == NULL)
        {
            ROS_ERROR("Cann't find codec!\n");
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        // A3.3 构建解码器AVCodecContext
        // A3.3.1 p_codec_ctx初始化：分配结构体，使用p_codec初始化相应成员为默认值
        p_codec_ctx = avcodec_alloc_context3(p_codec);
        if (p_codec_ctx == NULL)
        {
            ROS_ERROR("avcodec_alloc_context3() failed %d\n", ret);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }
        // A3.3.2 p_codec_ctx初始化：p_codec_par ==> p_codec_ctx，初始化相应成员
        ret = avcodec_parameters_to_context(p_codec_ctx, p_codec_par);
        if (ret < 0)
        {
            ROS_ERROR("avcodec_parameters_to_context() failed %d\n", ret);
            avcodec_free_context(&p_codec_ctx);

            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;

            return false;
        }
        // A3.3.3 p_codec_ctx初始化：使用p_codec初始化p_codec_ctx，初始化完成
        ret = avcodec_open2(p_codec_ctx, p_codec, NULL);
        if (ret < 0)
        {
            ROS_ERROR("avcodec_open2() failed %d\n", ret);
            avcodec_free_context(&p_codec_ctx);

            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

       
        if (p_packet == NULL)
        {  
            ROS_ERROR("av_malloc() failed\n");  
            avcodec_free_context(&p_codec_ctx);

            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        // B1. 初始化SDL子系统：缺省(事件处理、文件IO、线程)、视频、音频、定时器
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
        {  
            ROS_ERROR("SDL_Init() failed: %s\n", SDL_GetError()); 
            av_packet_unref(p_packet);
            avcodec_free_context(&p_codec_ctx);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        // 初始化队列，数据清零，创建互斥锁和条件变量
        packet_queue_init(&s_audio_pkt_queue);

        // B2. 打开音频设备并创建音频处理线程
        // B2.1 打开音频设备，获取SDL设备支持的音频参数actual_spec(期望的参数是wanted_spec，实际得到actual_spec)
        // 1) SDL提供两种使音频设备取得音频数据方法：
        //    a. push，SDL以特定的频率调用回调函数，在回调函数中取得音频数据
        //    b. pull，用户程序以特定的频率调用SDL_QueueAudio()，向音频设备提供数据。此种情况wanted_spec.callback=NULL
        // 2) 音频设备打开后播放静音，不启动回调，调用SDL_PauseAudio(0)后启动回调，开始正常播放音频
        wanted_spec.freq = p_codec_ctx->sample_rate;    // 采样率
        wanted_spec.format = AUDIO_S16SYS;              // S表带符号，16是采样深度，SYS表采用系统字节序
        wanted_spec.channels = p_codec_ctx->channels;   // 声道数
        wanted_spec.silence = 0;                        // 静音值
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;    // SDL声音缓冲区尺寸，单位是单声道采样点尺寸x通道数
        wanted_spec.callback = sdl_audio_callback;      // 回调函数，若为NULL，则应使用SDL_QueueAudio()机制
        wanted_spec.userdata = this;             // 提供给回调函数的参数
        if (SDL_OpenAudio(&wanted_spec, &actual_spec) < 0)
        {
            ROS_ERROR("SDL_OpenAudio() failed: %s\n", SDL_GetError());
            SDL_Quit();
            av_packet_unref(p_packet);
            avcodec_free_context(&p_codec_ctx);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }
        // B2.2 根据SDL音频参数构建音频重采样参数
        // wanted_spec是期望的参数，actual_spec是实际的参数，wanted_spec和auctual_spec都是SDL中的参数。
        // 此处audio_param是FFmpeg中的参数，此参数应保证是SDL播放支持的参数，后面重采样要用到此参数
        // 音频帧解码后得到的frame中的音频格式未必被SDL支持，比如frame可能是planar格式，但SDL2.0并不支持planar格式，
        // 若将解码后的frame直接送入SDL音频缓冲区，声音将无法正常播放。所以需要先将frame重采样(转换格式)为SDL支持的模式，
        // 然后送再写入SDL音频缓冲区
        s_audio_param_tgt.fmt = AV_SAMPLE_FMT_S16;
        //s_audio_param_tgt.fmt = (AVSampleFormat)actual_spec.format;
        s_audio_param_tgt.freq = actual_spec.freq;
        s_audio_param_tgt.channel_layout = av_get_default_channel_layout(actual_spec.channels);;
        s_audio_param_tgt.channels =  actual_spec.channels;
        s_audio_param_tgt.frame_size = av_samples_get_buffer_size(NULL, actual_spec.channels, 1, s_audio_param_tgt.fmt, 1);
        s_audio_param_tgt.bytes_per_sec = av_samples_get_buffer_size(NULL, actual_spec.channels, actual_spec.freq, s_audio_param_tgt.fmt, 1);
        
        
        if (s_audio_param_tgt.bytes_per_sec <= 0 || s_audio_param_tgt.frame_size <= 0)
        {
            ROS_ERROR("av_samples_get_buffer_size failed\n");
            SDL_Quit();
            
            (p_packet);
            avcodec_free_context(&p_codec_ctx);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }
        s_audio_param_src = s_audio_param_tgt;
        return true;
    }

    void Analysis(const string& input) 
    {
      control_msg.resize(0);
      std::istringstream stream(input);
      string token;
      char delimiter = '#';
      // 使用 ',' 作为定界符从字符串中读取内容，并存储到 tokens 容器中
      while (std::getline(stream, token, delimiter)) 
      {
        control_msg.push_back(token);
      }
    }

    void release()
    {
        SDL_Quit();

        // if(p_packet)
        // {
        //     //av_packet_unref(p_packet);
        //     av_packet_free(&p_packet);
        // }
            
        
        if(p_codec_ctx)
            avcodec_free_context(&p_codec_ctx);

        if(p_fmt_ctx)
        {
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
        }
            

        if(s_audio_swr_ctx)
            swr_free(&s_audio_swr_ctx);

        is_initial = false;
        std_msgs::String msg;
        msg.data = "close play";
        media_state_pub.publish(msg);
    }

  private:
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv;

    struct SwrContext *s_audio_swr_ctx;
    uint8_t *s_resample_buf = NULL;  // 重采样输出缓冲区
    unsigned s_resample_buf_len = 0;      // 重采样输出缓冲区长度

    bool is_input_finished = false;   // 文件读取完毕
    bool is_decode_finished = false;  // 解码完毕
    bool is_initial = false;

    AVCodecParameters*  p_codec_par = NULL;
    AVFormatContext*    p_fmt_ctx = nullptr;
    AVCodecContext*     p_codec_ctx = NULL;
    AVCodec*            p_codec = NULL;
    AVPacket*           p_packet = NULL;
    AVFrame*           p_frame = NULL;
    //AVPacket*           p_packet;
    SDL_AudioSpec       wanted_spec;
    SDL_AudioSpec       actual_spec;
    
    vector<string> control_msg;
    std::atomic<bool> is_rtsp_stream_coming ;
    ros::NodeHandle n;
    ros::Subscriber action_cmd_sub;
    ros::Publisher  media_state_pub;
    string rtsp_url;
    int a_idx = -1;
    std::function<void(int)> signalHandle;
};