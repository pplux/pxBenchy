#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include "gpu/vk.h"

namespace vkc {

// JSON writer for the Vulkan info: one member per line, two-space indent (short lines: logcat splits long ones).
// Members are written in call order; k = nullptr writes an array element.
struct Json {
  std::string out;
  int depth = 0;     // indent of the next member
  bool first = true; // no member written yet at this depth

  void key(const char* k); // separator, newline and indent, then "k": (nothing before the first value)
  void open(const char* k, char c = '{');
  void close(char c = '}');
  void boolean(const char* k, VkBool32 v);
  void str(const char* k, const char* v);                 // escaped
  void version(const char* k, uint32_t v);                // VK_MAKE_API_VERSION as "major.minor.patch"
  void put(const char* k, const VkConformanceVersion& v); // "major.minor.subminor.patch"
  // Numbers and enums; arrays: char as a string, uint8_t (UUIDs, LUIDs) as hex, the rest inline [a, b, c]
  template <class T> void put(const char* k, const T& v) {
    if constexpr (std::is_array_v<T>) {
      using E = std::remove_extent_t<T>;
      if constexpr (std::is_same_v<E, char>) {
        str(k, v);
      } else if constexpr (std::is_same_v<E, uint8_t>) {
        key(k);
        out += '"';
        for (uint8_t b : v) { number_text(b, 16); }
        out += '"';
      } else {
        key(k);
        out += '[';
        for (size_t i = 0; i < std::extent_v<T>; ++i) {
          if (i) { out += ", "; }
          value(v[i]);
        }
        out += ']';
      }
    } else {
      key(k);
      value(v);
    }
  }

private:
  template <class T> void value(T v) {
    if constexpr (std::is_enum_v<T>) {
      number_text((long long)v);
    } else if constexpr (std::is_floating_point_v<T>) {
      number_text((double)v);
    } else if constexpr (std::is_signed_v<T>) {
      number_text((long long)v);
    } else {
      number_text((unsigned long long)v);
    }
  }
  void number_text(long long v);
  void number_text(unsigned long long v);
  void number_text(double v);
  void number_text(uint8_t v, int base); // two hex digits
};

} // namespace vkc

namespace vkc::part {

// Instance-level members of the probe instance whose functions are in t (after probe_instance_create, so volk's
// global functions are loaded): instance_version, instance_extensions {name: spec version}, layers.
void info_instance(Json* j);

// Opens one physical device's object (array element) and writes the Vulkan 1.0 core part: properties, limits,
// sparse properties, features, memory heaps / types, queue families, device extensions and format properties of the
// core formats. The mode adds its own structs, then info_device_end closes it.
void info_device_begin(Json* j, VkPhysicalDevice pd, const VolkInstanceTable* t);
void info_device_end(Json* j);

} // namespace vkc::part
