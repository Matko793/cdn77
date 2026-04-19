#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/http/filter.h"

#include "source/common/common/logger.h"
#include "source/common/http/utility.h"
#include "source/extensions/filters/http/simple_cache/ring_buffer.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

namespace Envoy
{
  namespace Extensions
  {
    namespace HttpFilters
    {
      namespace SimpleCache
      {

        class Cache
        {
        public:
          explicit Cache(size_t ring_buffer_size) : m_ring_buffer_size(ring_buffer_size) {}

          CachedResponse *lookup(const std::string &key)
          {
            absl::MutexLock lock(&m_mutex);
            auto it = m_entries.find(key);
            if (it == m_entries.end())
            {
              return nullptr;
            }
            return it->second.latest();
          }

          void insert(const std::string &key, CachedResponse response)
          {
            absl::MutexLock lock(&m_mutex);
            auto [it, _] = m_entries.emplace(key, RingBuffer(m_ring_buffer_size));
            it->second.insert(std::move(response));
          }

        private:
          size_t m_ring_buffer_size;
          absl::Mutex m_mutex;
          absl::flat_hash_map<std::string, RingBuffer> m_entries ABSL_GUARDED_BY(m_mutex);
        };

        struct WaiterInfo
        {
          Http::StreamDecoderFilterCallbacks *callbacks;
          Event::Dispatcher *dispatcher;

          std::shared_ptr<std::atomic<bool>> alive;
        };

        struct CoalesceEntry
        {
          bool upstream_in_flight{false};
          std::vector<WaiterInfo> waiters;
        };

        class CoalesceMap
        {
        public:
          bool tryBecomePrimary(const std::string &key, Http::StreamDecoderFilterCallbacks *cb,
                                Event::Dispatcher *dispatcher,
                                std::shared_ptr<std::atomic<bool>> alive)
          {
            absl::MutexLock lock(&m_mutex);
            auto &entry = m_map[key];
            if (!entry.upstream_in_flight)
            {
              entry.upstream_in_flight = true;
              return true;
            }
            entry.waiters.push_back({cb, dispatcher, std::move(alive)});
            return false;
          }

          std::vector<WaiterInfo> getWaiters(const std::string &key)
          {
            absl::MutexLock lock(&m_mutex);
            auto it = m_map.find(key);
            if (it == m_map.end())
            {
              return {};
            }
            return it->second.waiters; // copy
          }

          void erase(const std::string &key)
          {
            absl::MutexLock lock(&m_mutex);
            m_map.erase(key);
          }

        private:
          absl::Mutex m_mutex;
          absl::flat_hash_map<std::string, CoalesceEntry> m_map ABSL_GUARDED_BY(m_mutex);
        };

        class SimpleCacheFilter : public Http::StreamFilter,
                                  public Logger::Loggable<Logger::Id::filter>
        {
        public:
          explicit SimpleCacheFilter(Cache &cache, CoalesceMap &coalesce_map);

          void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks &callbacks) override;
          Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap &headers,
                                                  bool end_stream) override;
          Http::FilterDataStatus decodeData(Buffer::Instance &data, bool end_stream) override;
          Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap &trailers) override;

          void setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks &callbacks) override;
          Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap &headers,
                                                  bool end_stream) override;
          Http::FilterDataStatus encodeData(Buffer::Instance &data, bool end_stream) override;
          Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap &trailers) override;

          Http::Filter1xxHeadersStatus encode1xxHeaders(Http::ResponseHeaderMap &) override
          {
            return Http::Filter1xxHeadersStatus::Continue;
          }
          Http::FilterMetadataStatus encodeMetadata(Http::MetadataMap &) override
          {
            return Http::FilterMetadataStatus::Continue;
          }

          void onDestroy() override { *m_alive = false; }

        private:
          std::string buildCacheKey(const Http::RequestHeaderMap &headers);
          void finalizeResponse();
          void replayCachedResponse(CachedResponse &entry, Http::StreamDecoderFilterCallbacks &cb);

          Cache &m_cache;
          CoalesceMap &m_coalesce_map;

          Http::StreamDecoderFilterCallbacks *m_decoder_callbacks{nullptr};
          Http::StreamEncoderFilterCallbacks *m_encoder_callbacks{nullptr};

          std::string m_cache_key;
          bool m_is_coalesced_waiter{false};

          std::shared_ptr<std::atomic<bool>> m_alive{std::make_shared<std::atomic<bool>>(true)};

          uint32_t m_response_status{0};
          Http::ResponseHeaderMapPtr m_response_headers;
          Buffer::OwnedImpl m_response_body;
        };

      } // namespace SimpleCache
    } // namespace HttpFilters
  } // namespace Extensions
} // namespace Envoy
