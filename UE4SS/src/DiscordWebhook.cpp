#ifdef __linux__
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <regex>

#include <DiscordWebhook.hpp>

namespace RC
{
    struct ParsedUrl
    {
        std::string host;
        std::string path;
        int port{443};
    };

    static auto parse_discord_url(const std::string& url) -> ParsedUrl
    {
        ParsedUrl result;
        // Expected format: https://discord.com/api/webhooks/ID/TOKEN
        // or https://discordapp.com/api/webhooks/ID/TOKEN
        std::regex url_regex(R"(https?://([^/]+)(/.+))");
        std::smatch match;
        if (std::regex_match(url, match, url_regex))
        {
            result.host = match[1].str();
            result.path = match[2].str();
            // Check if it's http or https
            if (url.substr(0, 5) == "http:")
            {
                result.port = 80;
            }
        }
        return result;
    }

    static auto url_escape_json(const std::string& s) -> std::string
    {
        std::string out;
        out.reserve(s.size() + 10);
        for (char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    static auto send_http_post(const std::string& host, const std::string& path, const std::string& json_body, int port) -> bool
    {
        // Resolve hostname
        struct hostent* he = gethostbyname(host.c_str());
        if (!he || he->h_addr_list[0] == nullptr)
        {
            fprintf(stderr, "[UE4SS] DiscordWebhook: failed to resolve host %s\n", host.c_str());
            return false;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            fprintf(stderr, "[UE4SS] DiscordWebhook: failed to create socket\n");
            return false;
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        // Set timeout
        struct timeval tv{};
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        {
            fprintf(stderr, "[UE4SS] DiscordWebhook: failed to connect to %s:%d\n", host.c_str(), port);
            close(sock);
            return false;
        }

        // Build HTTP POST request
        std::ostringstream request;
        request << "POST " << path << " HTTP/1.1\r\n";
        request << "Host: " << host << "\r\n";
        request << "Content-Type: application/json\r\n";
        request << "Content-Length: " << json_body.size() << "\r\n";
        request << "Connection: close\r\n";
        request << "\r\n";
        request << json_body;

        std::string req_str = request.str();
        ssize_t total_sent = 0;
        while (total_sent < static_cast<ssize_t>(req_str.size()))
        {
            ssize_t sent = send(sock, req_str.c_str() + total_sent, req_str.size() - total_sent, 0);
            if (sent <= 0)
            {
                fprintf(stderr, "[UE4SS] DiscordWebhook: failed to send data\n");
                close(sock);
                return false;
            }
            total_sent += sent;
        }

        // Read response (we just need to know it didn't error)
        char buf[4096];
        ssize_t received = recv(sock, buf, sizeof(buf) - 1, 0);
        close(sock);

        if (received <= 0)
        {
            fprintf(stderr, "[UE4SS] DiscordWebhook: no response received\n");
            return false;
        }

        buf[received] = '\0';
        // Check for HTTP 200/204
        if (strstr(buf, "HTTP/1.1 204") || strstr(buf, "HTTP/1.1 200") || strstr(buf, "HTTP/1.0 200"))
        {
            return true;
        }

        fprintf(stderr, "[UE4SS] DiscordWebhook: unexpected response: %.200s\n", buf);
        return false;
    }

    auto DiscordWebhook::send_embed(const std::string& webhook_url, const std::string& title, const std::string& description, int color) -> bool
    {
        if (webhook_url.empty())
        {
            return false;
        }

        ParsedUrl parsed = parse_discord_url(webhook_url);
        if (parsed.host.empty() || parsed.path.empty())
        {
            fprintf(stderr, "[UE4SS] DiscordWebhook: invalid URL format\n");
            return false;
        }

        // Build JSON payload with embed
        std::string json = "{\"embeds\":[{\"title\":\"";
        json += url_escape_json(title);
        json += "\",\"description\":\"";
        json += url_escape_json(description);
        json += "\",\"color\":";
        json += std::to_string(color);
        json += "}]}";

        return send_http_post(parsed.host, parsed.path, json, parsed.port);
    }

    auto DiscordWebhook::send_message(const std::string& webhook_url, const std::string& message) -> bool
    {
        if (webhook_url.empty())
        {
            return false;
        }

        ParsedUrl parsed = parse_discord_url(webhook_url);
        if (parsed.host.empty() || parsed.path.empty())
        {
            fprintf(stderr, "[UE4SS] DiscordWebhook: invalid URL format\n");
            return false;
        }

        std::string json = "{\"content\":\"";
        json += url_escape_json(message);
        json += "\"}";

        return send_http_post(parsed.host, parsed.path, json, parsed.port);
    }
} // namespace RC

#endif // __linux__
