#pragma once

#include "envoy/extensions/filters/http/simple_cache/v3/simple_cache.pb.h"
#include "envoy/extensions/filters/http/simple_cache/v3/simple_cache.pb.validate.h"

#include "source/extensions/filters/http/common/factory_base.h"
#include "source/extensions/filters/http/simple_cache/filter.h"

namespace Envoy
{
  namespace Extensions
  {
    namespace HttpFilters
    {
      namespace SimpleCache
      {

        class SimpleCacheFilterFactory
            : public Common::FactoryBase<
                  envoy::extensions::filters::http::simple_cache::v3::SimpleCache>
        {
        public:
          SimpleCacheFilterFactory() : FactoryBase("envoy.filters.http.simple_cache") {}

        private:
          Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
              const envoy::extensions::filters::http::simple_cache::v3::SimpleCache &proto_config,
              const std::string &stats_prefix,
              Server::Configuration::FactoryContext &context) override;
        };

        DECLARE_FACTORY(SimpleCacheFilterFactory);

      } // namespace SimpleCache
    } // namespace HttpFilters
  } // namespace Extensions
} // namespace Envoy
