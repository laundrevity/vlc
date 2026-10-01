/*****************************************************************************
 * chromecast_subtitles.h: native text subtitles for local Cast playback
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 *****************************************************************************/
#ifndef VLC_CHROMECAST_SUBTITLES_H
#define VLC_CHROMECAST_SUBTITLES_H

#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_httpd.h>
#include <vlc_input_item.h>
#include <memory>
#include <string>
#include <vector>

struct ChromecastSubtitleTrack
{
    unsigned id;
    std::string url;
    std::string language;
    std::string name;
};

class ChromecastSubtitles
{
public:
    ChromecastSubtitles(vlc_object_t *, httpd_host_t *, const std::string &root);
    ~ChromecastSubtitles();
    void SetInput(input_item_t *);
    bool Select(const es_format_t *);
    bool SelectId(int source_id);
    unsigned ActiveTrack();
    std::vector<ChromecastSubtitleTrack> Publish(const std::string &server, vlc_tick_t origin);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif
