/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#ifndef CAF_BINARY_SERIALIZER_HPP
#define CAF_BINARY_SERIALIZER_HPP

#include <utility>
#include <sstream>
#include <iomanip>
#include <functional>
#include <type_traits>

#include "caf/serializer.hpp"
#include "caf/primitive_variant.hpp"

#include "caf/detail/ieee_754.hpp"
#include "caf/detail/type_traits.hpp"

namespace caf {

/**
 * Implements the serializer interface with a binary serialization protocol.
 */
class binary_serializer : public serializer {
 public:
  using write_fun = std::function<void(const char*, const char*)>;

  /**
   * Creates a binary serializer writing to given iterator position.
   */
  template <class Iter>
  binary_serializer(Iter iter, actor_namespace* ns = nullptr)
      : serializer(ns),
        m_open_objects(0) {
    struct fun {
      fun(Iter pos) : m_pos(pos) {
        // nop
      }
      void operator()(const char* first, const char* last) {
        m_pos = std::copy(first, last, m_pos);
      }
      Iter m_pos;
    };
    m_out = fun{iter};
  }

  void begin_object(const uniform_type_info* uti) override;

  void end_object() override;

  void begin_sequence(size_t list_size) override;

  void end_sequence() override;

  void write_value(const primitive_variant& value) override;

  void write_raw(size_t num_bytes, const void* data) override;

 private:
  write_fun m_out;
  int m_open_objects;
};

template <class T,
          class = typename std::enable_if<detail::is_primitive<T>::value>::type>
binary_serializer& operator<<(binary_serializer& bs, const T& value) {
  bs.write_value(value);
  return bs;
}

} // namespace caf

#endif // CAF_BINARY_SERIALIZER_HPP
