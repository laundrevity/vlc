/*****************************************************************************
 * chromecast_webvtt.hpp: immutable native caption documents for Google Cast
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

#ifndef VLC_CHROMECAST_WEBVTT_HPP
#define VLC_CHROMECAST_WEBVTT_HPP

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace chromecast
{
struct Cue
{
    int64_t start;
    int64_t end;
    std::string text;
};

inline std::string EscapeText(const std::string &text)
{
    std::string result;
    for (char c : text)
        switch (c)
        {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '\r': break;
            case '\0': break;
            default: result += c;
        }
    // Blank lines terminate WebVTT cues. Preserve visual line breaks without
    // letting subtitle content introduce a new cue or a header.
    for (size_t i = 0; (i = result.find("\n\n", i)) != std::string::npos; i += 3)
        result.insert(i + 1, "\xe2\x80\x8b");
    return result;
}

inline std::string Timestamp(int64_t time)
{
    const int64_t ms = std::max<int64_t>(0, time) / 1000;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(2) << ms / 3600000 << ':'
        << std::setw(2) << ms / 60000 % 60 << ':'
        << std::setw(2) << ms / 1000 % 60 << '.' << std::setw(3) << ms % 1000;
    return out.str();
}

inline std::string Render(const std::vector<Cue> &cues, int64_t origin)
{
    std::string result = "WEBVTT\n\n";
    for (const Cue &cue : cues)
    {
        if (cue.end <= origin || cue.end <= cue.start || cue.text.empty())
            continue;
        const int64_t start = std::max<int64_t>(0, cue.start - origin);
        const int64_t end = cue.end - origin;
        if (end / 1000 <= start / 1000)
            continue;
        result += Timestamp(start) + " --> " + Timestamp(end) + '\n'
               + EscapeText(cue.text) + "\n\n";
    }
    return result;
}
}

#endif
