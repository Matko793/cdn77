#include "source/extensions/filters/http/simple_cache/filter.h"

#include "envoy/http/codes.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"

#include "absl/strings/str_cat.h"

namespace Envoy
{
  namespace Extensions
  {
    namespace HttpFilters
    {
      namespace SimpleCache
      {

        SimpleCacheFilter::SimpleCacheFilter(Cache &cache, CoalesceMap &coalesce_map)
            : m_cache(cache), m_coalesce_map(coalesce_map) {}

        void SimpleCacheFilter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks &callbacks)
        {
          m_decoder_callbacks = &callbacks;
        }
        void SimpleCacheFilter::setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks &callbacks)
        {
          m_encoder_callbacks = &callbacks;
        }

        std::string SimpleCacheFilter::buildCacheKey(const Http::RequestHeaderMap &headers)
        {
          return absl::StrCat(headers.getHostValue(), headers.getPathValue());
        }

        Http::FilterHeadersStatus SimpleCacheFilter::decodeHeaders(Http::RequestHeaderMap &headers,
                                                                   bool /*end_stream*/)
        {
          if (headers.getMethodValue() != "GET")
          {
            ENVOY_LOG(debug, "SimpleCache: skipping non-GET request");
            return Http::FilterHeadersStatus::Continue;
          }

          m_cache_key = buildCacheKey(headers);
          ENVOY_LOG(debug, "SimpleCache: cache key = {}", m_cache_key);

          CachedResponse *entry = m_cache.lookup(m_cache_key);
          if (entry != nullptr)
          {
            ENVOY_LOG(info, "SimpleCache: HIT for key={}", m_cache_key);
            replayCachedResponse(*entry, *m_decoder_callbacks);
            return Http::FilterHeadersStatus::StopIteration;
          }

          if (!m_coalesce_map.tryBecomePrimary(m_cache_key, m_decoder_callbacks,
                                               &m_decoder_callbacks->dispatcher(), m_alive))
          {
            ENVOY_LOG(info, "SimpleCache: coalescing request for key={}", m_cache_key);
            m_is_coalesced_waiter = true;
            return Http::FilterHeadersStatus::StopIteration;
          }

          ENVOY_LOG(info, "SimpleCache: MISS for key={}", m_cache_key);
          return Http::FilterHeadersStatus::Continue;
        }

        Http::FilterDataStatus SimpleCacheFilter::decodeData(Buffer::Instance & /*data*/,
                                                             bool /*end_stream*/)
        {
          return Http::FilterDataStatus::Continue;
        }
        Http::FilterTrailersStatus
        SimpleCacheFilter::decodeTrailers(Http::RequestTrailerMap & /*trailers*/)
        {
          return Http::FilterTrailersStatus::Continue;
        }

        Http::FilterHeadersStatus SimpleCacheFilter::encodeHeaders(Http::ResponseHeaderMap &headers,
                                                                   bool end_stream)
        {
          if (m_is_coalesced_waiter)
          {
            return Http::FilterHeadersStatus::Continue;
          }

          m_response_status = Http::Utility::getResponseStatus(headers);
          m_response_headers = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers);

          auto waiters = m_coalesce_map.getWaiters(m_cache_key);
          for (auto &w : waiters)
          {
            auto headers_copy = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(headers);
            w.dispatcher->post(
                [cb = w.callbacks, h = std::move(headers_copy), end_stream, alive = w.alive]() mutable
                {
                  if (*alive)
                  {
                    cb->encodeHeaders(std::move(h), end_stream, "simple_cache_coalesced_hit");
                  }
                });
          }

          if (end_stream)
          {
            finalizeResponse();
          }
          return Http::FilterHeadersStatus::Continue;
        }

        Http::FilterDataStatus SimpleCacheFilter::encodeData(Buffer::Instance &data, bool end_stream)
        {
          if (m_is_coalesced_waiter)
          {
            return Http::FilterDataStatus::Continue;
          }

          m_response_body.add(data);

          auto waiters = m_coalesce_map.getWaiters(m_cache_key);
          for (auto &w : waiters)
          {
            auto chunk = std::make_shared<Buffer::OwnedImpl>();
            chunk->add(data);
            w.dispatcher->post([cb = w.callbacks, c = std::move(chunk), end_stream,
                                alive = w.alive]() mutable
                               {
      if (*alive) {
        cb->encodeData(*c, end_stream);
      } });
          }

          if (end_stream)
          {
            finalizeResponse();
          }
          return Http::FilterDataStatus::Continue;
        }

        Http::FilterTrailersStatus
        SimpleCacheFilter::encodeTrailers(Http::ResponseTrailerMap &trailers)
        {
          if (m_is_coalesced_waiter)
          {
            return Http::FilterTrailersStatus::Continue;
          }

          auto waiters = m_coalesce_map.getWaiters(m_cache_key);
          for (auto &w : waiters)
          {
            auto trailers_copy = Http::createHeaderMap<Http::ResponseTrailerMapImpl>(trailers);
            w.dispatcher->post(
                [cb = w.callbacks, t = std::move(trailers_copy), alive = w.alive]() mutable
                {
                  if (*alive)
                  {
                    cb->encodeTrailers(std::move(t));
                  }
                });
          }

          finalizeResponse();
          return Http::FilterTrailersStatus::Continue;
        }

        void SimpleCacheFilter::finalizeResponse()
        {
          if (m_cache_key.empty())
          {
            return;
          }

          CachedResponse cached;
          cached.status_code = m_response_status;
          if (m_response_headers)
          {
            cached.headers = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*m_response_headers);
          }
          cached.body.add(m_response_body);
          cached.stored_at = std::chrono::steady_clock::now();

          m_cache.insert(m_cache_key, std::move(cached));
          ENVOY_LOG(info, "SimpleCache: stored response for key={}", m_cache_key);

          m_coalesce_map.erase(m_cache_key);
        }

        void SimpleCacheFilter::replayCachedResponse(CachedResponse &entry,
                                                     Http::StreamDecoderFilterCallbacks &cb)
        {
          cb.sendLocalReply(
              static_cast<Http::Code>(entry.status_code),
              entry.body.toString(),
              [&entry](Http::ResponseHeaderMap &resp_headers)
              {
                entry.headers->iterate([&resp_headers](const Http::HeaderEntry &header)
                                       {
          resp_headers.addCopy(
              Http::LowerCaseString(std::string(header.key().getStringView())),
              header.value().getStringView());
          return Http::HeaderMap::Iterate::Continue; });
              },
              absl::nullopt, "simple_cache_hit");
        }

      } // namespace SimpleCache
    } // namespace HttpFilters
  } // namespace Extensions
} // namespace Envoy
