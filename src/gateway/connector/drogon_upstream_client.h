#pragma once
#include "upstream_client.h"
#include <drogon/HttpClient.h>

namespace aegisgate {

class DrogonUpstreamClient : public UpstreamClient {
public:
    explicit DrogonUpstreamClient(const std::string& base_url);

    void send(UpstreamRequest req, ResponseCallback onDone,
              ErrorCallback onError) override;

    void sendStreaming(UpstreamRequest req, ChunkCallback onChunk,
                       ResponseCallback onDone,
                       ErrorCallback onError) override;

private:
    drogon::HttpClientPtr client_;
};

} // namespace aegisgate
