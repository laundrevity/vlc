/* Native Cast caption extraction and HTTP regression test. LGPL 2.1 or later. */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#undef NDEBUG
#include <cassert>
#include <fstream>
#include <iostream>
#include <vlc/vlc.h>
#include "../../../lib/libvlc_internal.h"
#include <vlc_common.h>
#include <vlc_httpd.h>
#include <vlc_input_item.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include "../../../modules/stream_out/chromecast/chromecast_subtitles.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

const char vlc_module_name[] = "chromecast_subtitles_test";

static std::string Fetch(vlc_object_t *owner, const std::string &url)
{
    stream_t *stream = vlc_stream_NewURL(owner, url.c_str());
    assert(stream);
    std::string result;
    char buffer[4096];
    ssize_t length;
    while ((length = vlc_stream_Read(stream, buffer, sizeof(buffer))) > 0)
        result.append(buffer, length);
    assert(length == 0);
    vlc_stream_Delete(stream);
    return result;
}

static std::string Request(unsigned port, const std::string &url, const char *method)
{
    const size_t path_start = url.find('/', url.find("://") + 3);
    std::string request = std::string(method) + ' ' + url.substr(path_start)
        + " HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://www.gstatic.com\r\n"
          "Access-Control-Request-Method: GET\r\nAccess-Control-Request-Headers: Range\r\n"
          "Access-Control-Request-Private-Network: true\r\nConnection: close\r\n\r\n";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    size_t sent = 0;
    while (sent < request.size())
    {
        ssize_t count = write(fd, request.data() + sent, request.size() - sent);
        assert(count > 0);
        sent += count;
    }
    std::string result;
    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) result.append(buffer, count);
    assert(count == 0);
    close(fd);
    return result;
}

int main(int argc, char **argv)
{
    alarm(30);
    setenv("VLC_PLUGIN_PATH", "../modules", 1);
    const char *options[] = { "--ignore-config", "--no-media-library", "--no-stats", "--no-video", "--no-audio" };
    libvlc_instance_t *instance = libvlc_new(sizeof(options) / sizeof(options[0]), options);
    assert(instance);
    vlc_object_t *owner = VLC_OBJECT(instance->p_libvlc_int);
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
    unsigned port = ntohs(address.sin_port);
    close(socket_fd);
    var_Create(owner, "http-host", VLC_VAR_STRING);
    var_SetString(owner, "http-host", "127.0.0.1");
    var_Create(owner, "http-port", VLC_VAR_INTEGER);
    var_SetInteger(owner, "http-port", port);
    httpd_host_t *host = vlc_http_HostNew(owner);
    assert(host);
    std::string server = "http://127.0.0.1:" + std::to_string(port);
    {
        ChromecastSubtitles subtitles(owner, host, "/test-native");
        const char *file = argc > 1 ? argv[1] : SRCDIR "/samples/chromecast.srt";
        char *uri = vlc_path2uri(file, NULL);
        assert(uri);
        input_item_t *item = input_item_New(uri, "caption test");
        free(uri);
        subtitles.SetInput(item);
        input_item_Release(item);
        const auto tracks = subtitles.Publish(server, 0);
        assert(!tracks.empty());
        if (argc == 1)
        {
            es_format_t format;
            es_format_Init(&format, SPU_ES, VLC_CODEC_SUBT);
            format.i_id = 0;
            assert(subtitles.Select(&format));
            assert(subtitles.ActiveTrack() == tracks[0].id);
            assert(subtitles.Select(NULL));
            assert(subtitles.ActiveTrack() == 0);
            assert(subtitles.Select(&format));
            es_format_Clean(&format);
        }
        const auto original = Fetch(owner, tracks[0].url);
        assert(original.compare(0, 8, "WEBVTT\n\n") == 0);
        assert(original.find(" --> ") != std::string::npos);
        const auto preflight = Request(port, tracks[0].url, "OPTIONS");
        assert(preflight.find("HTTP/1.1 204") == 0);
        assert(preflight.find("Access-Control-Allow-Origin: https://www.gstatic.com") != std::string::npos);
        assert(preflight.find("Access-Control-Allow-Private-Network: true") != std::string::npos);
        const auto generic = Request(port, server + "/unregistered", "OPTIONS");
        assert(generic.find("HTTP/1.1 200") == 0);
        assert(generic.find("Access-Control-Allow-Origin") == std::string::npos);
        const auto head = Request(port, tracks[0].url, "HEAD");
        assert(head.find("Content-Length: " + std::to_string(original.size())) != std::string::npos);
        assert(head.find("WEBVTT") == std::string::npos);
        const auto shifted = subtitles.Publish(server, 5000000);
        assert(shifted.size() == tracks.size());
        assert(shifted[0].url != tracks[0].url);
        assert(Fetch(owner, tracks[0].url) == original);
        assert(Fetch(owner, shifted[0].url) != original);
        if (argc > 2)
        {
            std::ofstream output(argv[2]);
            output << original;
            assert(output.good());
        }
        std::cout << "Native caption tracks: " << tracks.size()
                  << ", WebVTT bytes: " << original.size() << std::endl;
    }
    httpd_HostDelete(host);
    libvlc_release(instance);
    return 0;
}
