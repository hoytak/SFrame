/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */

#ifndef GRAPHLAB_FLEXIBLE_TYPE_FLEXIBLE_TYPE_CONVERSION_UTILS_HPP
#define GRAPHLAB_FLEXIBLE_TYPE_FLEXIBLE_TYPE_CONVERSION_UTILS_HPP

#include <vector>
#include <map>
#include <unordered_map>
#include <tuple>
#include <type_traits>
#include <util/code_optimization.hpp>
#include <flexible_type/flexible_type.hpp>
#include <flexible_type/type_traits.hpp>

namespace graphlab {

namespace flexible_type_internals {

static void throw_type_conversion_error(const flexible_type& val, const char *type) {
  std::ostringstream ss;
  ss << "Type conversion failure in flexible_type converter: expected "
     << type << "; got " << flex_type_enum_to_name(val.get_type());

  throw ss.str();
}

template <typename Arg>
static void __unpack_args(std::ostringstream& ss, Arg a) {
  ss << a;
}

template <typename Arg, typename... OtherArgs>
static void __unpack_args(std::ostringstream& ss, Arg a1, OtherArgs... a2) {
  ss << a1;
  __unpack_args(ss, a2...);
}

template <typename... PrintArgs>
static void throw_type_conversion_error(const flexible_type& val, const char* type, PrintArgs... args) {
  std::ostringstream ss;
  ss << "Type conversion failure in flexible_type converter: expected " << type;
  __unpack_args(ss, args...);
  ss << "; got " << flex_type_enum_to_name(val.get_type());

  throw ss.str();
}

////////////////////////////////////////////////////////////////////////////////

template <typename T>
static void __get_t(flexible_type& dest, const T& src) {
  convert_to_flexible_type(dest, src);
}

template <typename T>
static void __get_t(double& dest, const T& src) {
  dest = src;
}

template <size_t idx, typename C, typename... Args>
void __unpack_tuple(C& dest, const std::tuple<Args...>& src,
                    typename std::enable_if<idx == sizeof...(Args)>::type* = NULL) {}

template <size_t idx, typename C, typename... Args>
void __unpack_tuple(C& dest, const std::tuple<Args...>& src,
                    typename std::enable_if<idx != sizeof...(Args)>::type* = NULL) {
   __get_t(dest[idx], std::get<idx>(src));
   __unpack_tuple<idx + 1>(dest, src);
}

template <typename C, typename... Args>
static void unpack_tuple(C& dest, const std::tuple<Args...>& src) {
   __unpack_tuple<0>(dest, src);
}

////////////////////////////////////////////////////////////////////////////////


template <typename T>
static void __set_t(T& dest, const flexible_type& src) {
  convert_from_flexible_type(dest, src);
}

template <typename T>
static void __set_t(T& dest, double src) {
  dest = static_cast<T>(src);
}

template <size_t idx, typename C, typename... Args>
void __pack_tuple(std::tuple<Args...>& dest, const C& src,
                    typename std::enable_if<idx == sizeof...(Args)>::type* = NULL) {}

template <size_t idx, typename C, typename... Args>
void __pack_tuple(std::tuple<Args...>& dest, const C& src,
                    typename std::enable_if<idx != sizeof...(Args)>::type* = NULL) {
   __set_t(std::get<idx>(dest), src[idx]);
   __pack_tuple<idx + 1>(dest, src);
}

template <typename C, typename... Args>
static void pack_tuple(std::tuple<Args...>& dest, const C& src) {
   __pack_tuple<0>(dest, src);
}

}}


#endif
