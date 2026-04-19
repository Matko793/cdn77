#pragma once

#include <chrono>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/http/header_map.h"

#include "source/common/buffer/buffer_impl.h"

namespace Envoy
{
  namespace Extensions
  {
    namespace HttpFilters
    {
      namespace SimpleCache
      {

        struct CachedResponse
        {
          uint32_t status_code{0};
          Envoy::Http::ResponseHeaderMapPtr headers;
          Envoy::Buffer::OwnedImpl body;
          std::chrono::steady_clock::time_point stored_at;
        };

        class RingBuffer
        {
        public:
          explicit RingBuffer(size_t capacity) : m_capacity(capacity), m_slots(capacity) {}

          void insert(CachedResponse response)
          {
            m_slots[m_head] = std::move(response);
            m_head = (m_head + 1) % m_capacity;
            if (m_size < m_capacity)
            {
              ++m_size;
            }
          }

          CachedResponse *latest()
          {
            if (m_size == 0)
            {
              return nullptr;
            }
            size_t index = (m_head + m_capacity - 1) % m_capacity;
            return &m_slots[index];
          }

        private:
          size_t m_capacity;
          size_t m_head{0};
          size_t m_size{0};
          std::vector<CachedResponse> m_slots;
        };

      } // namespace SimpleCache
    } // namespace HttpFilters
  } // namespace Extensions
} // namespace Envoy
