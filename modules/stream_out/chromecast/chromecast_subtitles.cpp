/*****************************************************************************
 * chromecast_subtitles.cpp: native text subtitles for local Cast playback
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast_subtitles.h"
#include "chromecast_webvtt.hpp"
#include <vlc_block.h>
#include <vlc_codec.h>
#include <vlc_demux.h>
#include <vlc_es_out.h>
#include <vlc_interrupt.h>
#include <vlc_meta.h>
#include <vlc_modules.h>
#include <vlc_stream.h>
#include <vlc_subpicture.h>
#include <vlc_text_style.h>
#include <vlc_url.h>
#include <algorithm>
#include <deque>
#include <set>
#include <vector>

namespace
{
const size_t MAX_CAPTION_BYTES = 16 * 1024 * 1024;
const size_t MAX_CUES = 100000;

bool Supported(vlc_fourcc_t codec)
{
    return codec == VLC_CODEC_SUBT || codec == VLC_CODEC_TX3G || codec == VLC_CODEC_WEBVTT;
}

struct Track
{
    es_format_t format;
    decoder_t *decoder = NULL;
    std::vector<chromecast::Cue> cues;
    size_t bytes = 0;
    bool failed = false;
    bool external = false;

    Track(const es_format_t *fmt, int id)
    {
        es_format_Copy(&format, fmt);
        format.i_id = id;
    }
    ~Track()
    {
        if (decoder)
        {
            if (decoder->p_module)
                module_unneed(decoder, decoder->p_module);
            if (decoder->p_description)
                vlc_meta_Delete(decoder->p_description);
            es_format_Clean(&decoder->fmt_in);
            es_format_Clean(&decoder->fmt_out);
            vlc_object_release(decoder);
        }
        es_format_Clean(&format);
    }
};

subpicture_t *NewSubpicture(decoder_t *, const subpicture_updater_t *updater)
{
    subpicture_t *picture = subpicture_New(updater);
    if (picture)
        picture->b_subtitle = true;
    return picture;
}

int QueueSubpicture(decoder_t *decoder, subpicture_t *picture)
{
    Track *track = static_cast<Track *>(decoder->p_queue_ctx);
    try
    {
        video_format_t video;
        video_format_Init(&video, VLC_CODEC_I420);
        video.i_width = video.i_visible_width = 1920;
        video.i_height = video.i_visible_height = 1080;
        video.i_sar_num = video.i_sar_den = 1;
        subpicture_Update(picture, &video, &video, picture->i_start);
        video_format_Clean(&video);
        std::string text;
        for (subpicture_region_t *region = picture->p_region; region; region = region->p_next)
        {
            if (!text.empty() && region->p_text)
                text += '\n';
            for (text_segment_t *segment = region->p_text; segment; segment = segment->p_next)
                if (segment->psz_text)
                    text += segment->psz_text;
        }
        if (!text.empty() && picture->i_start >= VLC_TICK_0 && picture->i_stop > picture->i_start)
        {
            track->bytes += text.size();
            if (track->bytes > MAX_CAPTION_BYTES || track->cues.size() >= MAX_CUES)
                track->failed = true;
            else
                track->cues.push_back({ picture->i_start - VLC_TICK_0,
                                        picture->i_stop - VLC_TICK_0, std::move(text) });
        }
    }
    catch (const std::bad_alloc &)
    {
        track->failed = true;
    }
    subpicture_Delete(picture);
    return track->failed ? VLC_EGENERIC : VLC_SUCCESS;
}

// An independent, unpaced demux pass collects only text subtitle streams.
// Reusing VLC's demuxers preserves its track IDs, edit lists and subtitle decoders.
struct Collector
{
    es_out_t out;
    vlc_object_t *owner;
    std::vector<std::unique_ptr<Track>> tracks;
    int next_id;
    bool failed = false;

    Collector(vlc_object_t *obj, int first_id) : owner(obj), next_id(first_id)
    {
        out.p_sys = reinterpret_cast<es_out_sys_t *>(this);
        out.pf_add = Add;
        out.pf_send = Send;
        out.pf_del = Del;
        out.pf_control = Control;
        out.pf_destroy = Destroy;
    }

    static es_out_id_t *Add(es_out_t *out, const es_format_t *fmt)
    {
        Collector *self = reinterpret_cast<Collector *>(out->p_sys);
        try
        {
            std::unique_ptr<Track> track(new Track(fmt, fmt->i_id < 0 ? self->next_id : fmt->i_id));
            ++self->next_id;
            if (fmt->i_cat == VIDEO_ES && fmt->video.i_frame_rate && fmt->video.i_frame_rate_base)
                var_SetFloat(self->owner, "sub-original-fps",
                             double(fmt->video.i_frame_rate) / fmt->video.i_frame_rate_base);
            if (fmt->i_cat == SPU_ES && Supported(fmt->i_codec))
            {
                track->decoder = static_cast<decoder_t *>(vlc_object_create(self->owner, sizeof(decoder_t)));
                if (!track->decoder)
                    return NULL;
                decoder_t *dec = track->decoder;
                es_format_Copy(&dec->fmt_in, fmt);
                es_format_Init(&dec->fmt_out, SPU_ES, 0);
                dec->b_frame_drop_allowed = false;
                dec->pf_spu_buffer_new = NewSubpicture;
                dec->pf_queue_sub = QueueSubpicture;
                dec->p_queue_ctx = track.get();
                dec->p_module = module_need(dec, "spu decoder", "$codec", false);
            }
            es_out_id_t *id = reinterpret_cast<es_out_id_t *>(track.get());
            self->tracks.push_back(std::move(track));
            return id;
        }
        catch (const std::bad_alloc &)
        {
            self->failed = true;
            return NULL;
        }
    }

    static int Send(es_out_t *, es_out_id_t *id, block_t *block)
    {
        Track *track = reinterpret_cast<Track *>(id);
        if (track->decoder && track->decoder->p_module && !track->failed)
            return track->decoder->pf_decode(track->decoder, block);
        block_Release(block);
        return VLC_SUCCESS;
    }

    static void Del(es_out_t *, es_out_id_t *) {} // Keep collected cues after demux closes.
    static void Destroy(es_out_t *) {}
    static int Control(es_out_t *, int query, va_list args)
    {
        switch (query)
        {
            case ES_OUT_GET_ES_STATE:
            {
                Track *track = reinterpret_cast<Track *>(va_arg(args, es_out_id_t *));
                *va_arg(args, bool *) = track && track->decoder && track->decoder->p_module;
                return VLC_SUCCESS;
            }
            case ES_OUT_GET_EMPTY:
                *va_arg(args, bool *) = true;
                return VLC_SUCCESS;
            case ES_OUT_SET_PCR:
            case ES_OUT_SET_GROUP_PCR:
            case ES_OUT_RESET_PCR:
            case ES_OUT_SET_ES:
            case ES_OUT_SET_ES_DEFAULT:
            case ES_OUT_SET_ES_STATE:
            case ES_OUT_SET_GROUP:
            case ES_OUT_SET_GROUP_META:
            case ES_OUT_SET_META:
            case ES_OUT_SET_NEXT_DISPLAY_TIME:
                return VLC_SUCCESS;
            default:
                return VLC_EGENERIC;
        }
    }
};

struct CaptionAsset
{
    httpd_url_t *endpoint = NULL;
    std::string path;
    std::string body;
    ~CaptionAsset() { if (endpoint) httpd_UrlDelete(endpoint); }

    static int Serve(httpd_callback_sys_t *data, httpd_client_t *,
                     httpd_message_t *answer, const httpd_message_t *query)
    {
        if (!answer || !query)
            return VLC_SUCCESS;
        CaptionAsset *self = reinterpret_cast<CaptionAsset *>(data);
        answer->i_proto = HTTPD_PROTO_HTTP;
        answer->i_version = 1;
        answer->i_type = HTTPD_MSG_ANSWER;
        answer->i_status = query->i_type == HTTPD_MSG_OPTIONS ? 204 : 200;
        const char *origin = httpd_MsgGet(query, "Origin");
        httpd_MsgAdd(answer, "Access-Control-Allow-Origin", "%s", origin && *origin ? origin : "https://www.gstatic.com");
        httpd_MsgAdd(answer, "Vary", "Origin");
        httpd_MsgAdd(answer, "Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
        httpd_MsgAdd(answer, "Access-Control-Allow-Headers", "Range, Content-Type, Accept-Encoding");
        const char *private_network = httpd_MsgGet(query, "Access-Control-Request-Private-Network");
        if (private_network && !strcmp(private_network, "true"))
            httpd_MsgAdd(answer, "Access-Control-Allow-Private-Network", "true");
        httpd_MsgAdd(answer, "Content-Type", "text/vtt; charset=utf-8");
        httpd_MsgAdd(answer, "Cache-Control", "no-store");
        httpd_MsgAdd(answer, "Connection", "close");
        httpd_MsgAdd(answer, "Content-Length", "%zu", query->i_type == HTTPD_MSG_OPTIONS ? 0 : self->body.size());
        if (query->i_type == HTTPD_MSG_GET)
        {
            answer->p_body = static_cast<uint8_t *>(malloc(self->body.size()));
            if (!answer->p_body)
            {
                answer->i_status = 500;
                return VLC_ENOMEM;
            }
            memcpy(answer->p_body, self->body.data(), self->body.size());
            answer->i_body = self->body.size();
        }
        return VLC_SUCCESS;
    }
};
}

struct ChromecastSubtitles::Impl
{
    vlc_object_t *owner;
    httpd_host_t *host;
    std::string root;
    vlc_mutex_t lock;
    input_item_t *item = NULL;
    std::set<std::string> scanned;
    std::vector<std::unique_ptr<Track>> tracks;
    std::deque<std::unique_ptr<CaptionAsset>> assets;
    Track *selected = NULL;
    int next_id = 0;
    unsigned generation = 0;

    Impl(vlc_object_t *obj, httpd_host_t *server, const std::string &prefix)
        : owner(obj), host(server), root(prefix)
    {
        vlc_mutex_init(&lock);
        var_Create(owner, "sub-original-fps", VLC_VAR_FLOAT);
    }
    ~Impl()
    {
        if (item) input_item_Release(item);
        var_Destroy(owner, "sub-original-fps");
        vlc_mutex_destroy(&lock);
    }

    void Load(const std::string &uri, bool external)
    {
        if (uri.compare(0, 7, "file://") != 0 || !scanned.insert(uri).second)
            return;
        Collector collector(owner, next_id);
        stream_t *stream = vlc_stream_NewURL(owner, uri.c_str());
        if (!stream)
            return;
        const char *extension = strrchr(uri.c_str(), '.');
        const bool plain_text = extension && (!strcasecmp(extension, ".srt") || !strcasecmp(extension, ".sub"));
        demux_t *demux = demux_New(owner, plain_text ? "subtitle" : "any",
                                  uri.c_str() + 7, stream, &collector.out);
        if (!demux)
        {
            vlc_stream_Delete(stream);
            return;
        }
        // Subtitle-only demuxers otherwise wait for the parent's playback clock.
        demux_Control(demux, DEMUX_SET_NEXT_DEMUX_TIME, INT64_MAX);
        const vlc_tick_t deadline = mdate() + 10 * CLOCK_FREQ;
        int status = VLC_DEMUXER_SUCCESS;
        while (status == VLC_DEMUXER_SUCCESS && !collector.failed && !vlc_killed() && mdate() < deadline)
            status = demux_Demux(demux);
        demux_Delete(demux);
        next_id = collector.next_id;
        if (status != VLC_DEMUXER_EOF || collector.failed)
        {
            msg_Warn(owner, "Native subtitle scan did not finish; ignoring incomplete captions");
            return;
        }
        for (auto &track : collector.tracks)
        {
            if (track->failed || track->cues.empty())
                continue;
            track->external = external;
            msg_Dbg(owner, "Prepared %zu native subtitle cues for track %d", track->cues.size(), track->format.i_id);
            tracks.push_back(std::move(track));
        }
    }

    void Refresh()
    {
        if (!item) return;
        char *uri = input_item_GetURI(item);
        if (uri) { std::string source(uri); free(uri); Load(source, false); }
        std::vector<std::string> slaves;
        {
            vlc_mutex_locker locker(&item->lock);
            for (int i = 0; i < item->i_slaves; ++i)
                if (item->pp_slaves[i]->i_type == SLAVE_TYPE_SPU)
                    slaves.push_back(item->pp_slaves[i]->psz_uri);
        }
        for (const auto &slave : slaves)
            Load(slave, true);
    }
};

ChromecastSubtitles::ChromecastSubtitles(vlc_object_t *owner, httpd_host_t *host,
                                       const std::string &root)
    : impl(new Impl(owner, host, root)) {}
ChromecastSubtitles::~ChromecastSubtitles() = default;

void ChromecastSubtitles::SetInput(input_item_t *item)
{
    vlc_mutex_locker locker(&impl->lock);
    if (impl->item == item) return;
    if (item) input_item_Hold(item);
    if (impl->item) input_item_Release(impl->item);
    impl->item = item;
    impl->selected = NULL;
    impl->tracks.clear();
    impl->scanned.clear();
    impl->next_id = 0;
    impl->Refresh();
}

bool ChromecastSubtitles::Select(const es_format_t *format)
{
    vlc_mutex_locker locker(&impl->lock);
    Track *selected = NULL;
    if (format && Supported(format->i_codec))
    {
        impl->Refresh();
        for (const auto &track : impl->tracks)
            if (track->format.i_id == format->i_id && track->format.i_codec == format->i_codec)
                selected = track.get();
        if (!selected)
            msg_Warn(impl->owner, "No complete native captions for selected subtitle track %d", format->i_id);
    }
    const bool changed = impl->selected != selected;
    impl->selected = selected;
    return changed;
}

unsigned ChromecastSubtitles::ActiveTrack()
{
    vlc_mutex_locker locker(&impl->lock);
    for (size_t i = 0; i < impl->tracks.size(); ++i)
        if (impl->tracks[i].get() == impl->selected)
            return i + 1;
    return 0;
}

bool ChromecastSubtitles::SelectId(int source_id)
{
    vlc_mutex_locker locker(&impl->lock);
    if (source_id == -SPU_ES)
    {
        impl->selected = NULL;
        return true;
    }
    impl->Refresh();
    for (const auto &track : impl->tracks)
        if (track->format.i_id == source_id)
        {
            impl->selected = track.get();
            return true;
        }
    return false;
}

std::vector<ChromecastSubtitleTrack> ChromecastSubtitles::Publish(const std::string &server, vlc_tick_t origin)
{
    vlc_mutex_locker locker(&impl->lock);
    std::vector<ChromecastSubtitleTrack> result;
    for (size_t i = 0; i < impl->tracks.size(); ++i)
    {
        std::unique_ptr<CaptionAsset> asset(new CaptionAsset);
        asset->path = impl->root + "-captions-" + std::to_string(++impl->generation) + ".vtt";
        asset->body = chromecast::Render(impl->tracks[i]->cues, origin);
        if (asset->body.size() > MAX_CAPTION_BYTES) continue;
        asset->endpoint = httpd_UrlNew(impl->host, asset->path.c_str(), NULL, NULL);
        if (!asset->endpoint) continue;
        for (int method : { HTTPD_MSG_GET, HTTPD_MSG_HEAD, HTTPD_MSG_OPTIONS })
            httpd_UrlCatch(asset->endpoint, method, CaptionAsset::Serve,
                          reinterpret_cast<httpd_callback_sys_t *>(asset.get()));
        const es_format_t &format = impl->tracks[i]->format;
        result.push_back({ static_cast<unsigned>(i + 1), server + asset->path,
            format.psz_language ? format.psz_language : "und",
            format.psz_description ? format.psz_description : "Subtitles" });
        impl->assets.push_back(std::move(asset));
    }
    // Retain one previous LOAD's immutable assets for requests already in flight.
    while (impl->assets.size() > 2 * impl->tracks.size()) impl->assets.pop_front();
    msg_Dbg(impl->owner, "Published %zu native caption tracks at origin %.3fs", result.size(), origin / double(CLOCK_FREQ));
    return result;
}
