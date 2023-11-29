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

#include <pthread.h>
#include <stdio.h>

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
        ros::spin();
    }
    ~ParseRtsp()
    {
        is_rtsp_stream_coming = false;
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
            cout <<"入队列时，内存申请失败" <<endl;
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
                        cout <<"s_resample_buf 申请失败" <<endl;
                        return AVERROR(ENOMEM);
                    }
                    // 音频重采样：返回值是重采样后得到的音频数据中单个声道的样本数
                    nb_samples = swr_convert(s_audio_swr_ctx, out, out_count, in, p_frame->nb_samples);
                    if (nb_samples < 0) {
                        ROS_ERROR("swr_convert() failed\n");
                        cout <<"swr_convert() failed" <<endl;
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
                    cout <<"frame size = "<< frm_size << "buffer size  = " <<  buf_size <<endl;
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
                    cout <<"向解码器喂数据失败" <<endl;
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
                cout <<"队列为空" <<endl;
                ret = 0;
                break;
            }
            else if (!block)            // 队列空且阻塞标志无效，则立即退出
            {
                //cout <<"队列空且阻塞标志无效，则立即退出" <<endl;
                ret = 0;
                break;
            }
            else                        // 队列空且阻塞标志有效，则等待
            {
                if(is_rtsp_stream_coming == false)
                {
                    ret = 0;
                    break;
                }
                    
                cout <<"wait" <<endl;
                SDL_CondWait(q->cond, q->mutex);
                cout <<"begin" <<endl;
            }
        }
        SDL_UnlockMutex(q->mutex);
        return ret;
    }

    // 双声道采样点的顺序为LRLRLR
    void static sdl_audio_callback(void *userdata, uint8_t *stream, int len)
    {
        ParseRtsp* rtsp = static_cast<ParseRtsp*>(userdata);
        
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

        if(len == 0)
        {}

        while (len > 0)         // 确保stream缓冲区填满，填满后此函数返回
        {
            if (s_tx_idx >= s_audio_len)
            {   
                // 1. 从队列中读出一包音频数据
                if (rtsp->packet_queue_pop(&s_audio_pkt_queue, packet, 0) <= 0)
                {
                    if (rtsp->is_input_finished)
                    {
                        av_packet_unref(packet);
                        ROS_INFO("Flushing decoder...\n");
                    }
                    else
                    {
                        if(packet)
                        {
                            av_freep(&packet);
                            //av_packet_free(&packet);
                            packet = NULL;
                        }
                        return;
                    }
                }

                get_size = rtsp->audio_decode_frame(rtsp->p_codec_ctx, packet, s_audio_buf, sizeof(s_audio_buf));
                if (get_size < 0)
                {
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
                {}
            }

            copy_len = s_audio_len - s_tx_idx;
            if (copy_len > len)
            {
                copy_len = len;
            }

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
        if(!init_codec_sdl()) 
        {
            ROS_ERROR("初始化失败，线程结束");
            is_rtsp_stream_coming = false;
            return ;
        }

        SDL_PauseAudio(0);
        
        while (is_rtsp_stream_coming && av_read_frame(p_fmt_ctx, p_packet) == 0)
        {
            if (p_packet->stream_index == a_idx)
            {
                packet_queue_push(&s_audio_pkt_queue, p_packet);
            }
            else
            {
                av_packet_unref(p_packet);
            }
        }
        
        if(is_rtsp_stream_coming == true) 
        {
            std_msgs::String msg;
            msg.data = "failed to play";
            media_state_pub.publish(msg);
            is_rtsp_stream_coming = false;
        }
       
        SDL_PauseAudio(1);
        release();
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
            if(is_rtsp_stream_coming)
                return ;
                
            cout <<"收到开启命令，准备开启" <<endl;
            is_rtsp_stream_coming = true;
            rtsp_url = control_msg[2];
            thread_ = std::thread(std::bind(&ParseRtsp::recv_rtsp_stream, this));
            thread_.detach();
        }
        else if(control_msg[0] == "stop_media_pull")
        {
            is_rtsp_stream_coming = false;
            cout <<"收到关闭命令，准备关闭" <<endl;
        }
        control_msg.clear();
    }
    
    bool init_codec_sdl()
    {
        s_audio_swr_ctx = swr_alloc();

        if(p_fmt_ctx != nullptr)
        {
            p_fmt_ctx = nullptr;
        }

        int ret = avformat_open_input(&p_fmt_ctx, rtsp_url.c_str() , NULL, NULL);
        if (ret != 0)
        {
            ROS_ERROR("avformat_open_input() failed %d\n", ret);
  
            if (s_resample_buf != NULL)
            {
                av_free(s_resample_buf);
            }
            return false;
        }

        ret = avformat_find_stream_info(p_fmt_ctx, NULL);
        if (ret < 0)
        {
            ROS_ERROR("avformat_find_stream_info() failed %d\n", ret);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        av_dump_format(p_fmt_ctx, 0, nullptr, 0);

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
            p_fmt_ctx = nullptr;
            return false;
        }

        p_codec_par = p_fmt_ctx->streams[a_idx]->codecpar;
        p_codec = avcodec_find_decoder(p_codec_par->codec_id);
        if (p_codec == NULL)
        {
            ROS_ERROR("Cann't find codec!\n");
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        p_codec_ctx = avcodec_alloc_context3(p_codec);
        if (p_codec_ctx == NULL)
        {
            ROS_ERROR("avcodec_alloc_context3() failed %d\n", ret);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }
       
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
        
        
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
        {  
            ROS_ERROR("SDL_Init() failed: %s\n", SDL_GetError()); 
            av_packet_unref(p_packet);
            avcodec_free_context(&p_codec_ctx);
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
            return false;
        }

        packet_queue_init(&s_audio_pkt_queue);
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
            SDL_CloseAudio();
            SDL_Quit();
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
        //cout <<"执行清理工作" <<endl;
        SDL_CloseAudio();
        SDL_Quit();  

        if(p_codec_ctx)
        {
            avcodec_free_context(&p_codec_ctx);
        }
       
        
        if(p_fmt_ctx)
        {
            //cout << "清理 p_fmt_ctx" <<endl;
            avformat_close_input(&p_fmt_ctx);
            p_fmt_ctx = nullptr;
        }
        
        if(s_audio_swr_ctx)
        {
            //cout << "清理 s_audio_swr_ctx" <<endl;
            swr_free(&s_audio_swr_ctx);
            s_audio_swr_ctx = nullptr;
        }
        
        
        is_initial = false;
    }

  private:
    std::thread thread_;
    std::thread thread_test;
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
     std::string rtsp_url;
    int a_idx = -1;
    std::function<void(int)> signalHandle;

    uint32_t count_ = 0;

    bool test = false;
    bool is_stop_audio_device = false;
};