/* Native Chromecast caption timing regression tests. LGPL 2.1 or later. */
#undef NDEBUG
#include <cassert>
#include "../../../modules/stream_out/chromecast/chromecast_webvtt.hpp"

int main()
{
    using namespace chromecast;
    const std::vector<Cue> cues = {
        { 1000000, 3000000, "First line" },
        { 3000000, 8000000, "Across a seek" },
        { 10000000, 12000000, "Later line" },
    };
    const auto original = Render(cues, 0);
    assert(original.find("00:00:01.000 --> 00:00:03.000\nFirst line") != std::string::npos);
    const auto seek = Render(cues, 5000000);
    assert(seek.find("First line") == std::string::npos);
    assert(seek.find("00:00:00.000 --> 00:00:03.000\nAcross a seek") != std::string::npos);
    assert(seek.find("00:00:05.000 --> 00:00:07.000\nLater line") != std::string::npos);
    assert(Render(cues, 3000000).find("First line") == std::string::npos);
    assert(Render(cues, 13000000) == "WEBVTT\n\n");
    assert(Render(cues, 0) == original); // Rewinds do not mutate the cached cues.
    assert(Render(cues, -83000).find("00:00:01.083 --> 00:00:03.083") != std::string::npos);
    assert(Timestamp(3600123000LL) == "01:00:00.123");
    assert(Timestamp(360000000000LL) == "100:00:00.000");
    assert(Timestamp(-1000) == "00:00:00.000");
    const auto escaped = EscapeText("A & B <b>literal</b>\n\nsecond\n\n\nthird");
    assert(escaped.find("&amp;") != std::string::npos);
    assert(escaped.find("&lt;b&gt;literal&lt;/b&gt;") != std::string::npos);
    assert(escaped.find("\n\n") == std::string::npos);
    assert(Render({{ 1, 2, "too short" }, { 20, 1, "backwards" }}, 0) == "WEBVTT\n\n");
    return 0;
}
