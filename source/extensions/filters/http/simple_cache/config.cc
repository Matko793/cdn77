#include "source/extensions/filters/http/simple_cache/config.h"

#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/simple_cache/filter.h"

namespace Envoy
{
  namespace Extensions
  {
    namespace HttpFilters
    {
      namespace SimpleCache
      {

        Http::FilterFactoryCb SimpleCacheFilterFactory::createFilterFactoryFromProtoTyped(
            const envoy::extensions::filters::http::simple_cache::v3::SimpleCache &proto_config,
            const std::string & /*stats_prefix*/,
            Server::Configuration::FactoryContext & /*context*/)
        {

          const size_t ring_buffer_size =
              proto_config.ring_buffer_size() > 0 ? proto_config.ring_buffer_size() : 1;

          auto cache = std::make_shared<Cache>(ring_buffer_size);
          auto coalesce_map = std::make_shared<CoalesceMap>();

          return [cache, coalesce_map](Http::FilterChainFactoryCallbacks &callbacks)
          {
            callbacks.addStreamFilter(
                std::make_shared<SimpleCacheFilter>(*cache, *coalesce_map));
          };
        }

        REGISTER_FACTORY(SimpleCacheFilterFactory,
                         Server::Configuration::NamedHttpFilterConfigFactory);

      } // namespace SimpleCache
    } // namespace HttpFilters
  } // namespace Extensions
} // namespace Envoy
