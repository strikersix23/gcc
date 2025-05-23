// Copyright (C) 2018-2025 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

// { dg-do compile { target c++20 } }

#include <bit>
#include <limits>

template<typename UInt>
constexpr auto
test(UInt x)
-> decltype(std::bit_width(x))
{
  static_assert( noexcept(std::bit_width(x)) );

  static_assert( std::bit_width(UInt(0)) == 0 );
  static_assert( std::bit_width(UInt(1)) == 1 );
  static_assert( std::bit_width(UInt(2)) == 2 );
  static_assert( std::bit_width(UInt(3)) == 2 );
  static_assert( std::bit_width(UInt(4)) == 3 );
  static_assert( std::bit_width(UInt(0x11)) == 5 );
  static_assert( std::bit_width(UInt(0x20)) == 6 );

  if constexpr (std::numeric_limits<UInt>::digits > 8)
  {
    static_assert( std::bit_width(UInt(0x201)) == 10 );
    static_assert( std::bit_width(UInt(0x8ff)) == 12 );
    static_assert( std::bit_width(UInt(0x1000)) == 13 );
  }

  if constexpr (std::numeric_limits<UInt>::digits > 32)
  {
    static_assert( std::bit_width(UInt(0xabcdef)) == 24 );
    static_assert( std::bit_width(UInt(0x1000000)) == 25 );
    static_assert( std::bit_width(UInt(0x1000001)) == 25 );
  }

  if constexpr (std::numeric_limits<UInt>::digits > 64)
  {
    static_assert( std::bit_width(UInt(1) << 64) == 65 );
    static_assert( std::bit_width(UInt(3) << 64) == 66 );
  }

  return true;
}

static_assert( test( (unsigned char)0 ) );
static_assert( test( (unsigned short)0 ) );
static_assert( test( (unsigned int)0 ) );
static_assert( test( (unsigned long)0 ) );
static_assert( test( (unsigned long long)0 ) );

// std::bit_width(T) shall not participate in overload resolution
// unless T is an unsigned integer type.
struct X { constexpr bool did_not_match() { return true; } };
constexpr X test(...) { return X{}; }
static_assert( test( (bool)0 ).did_not_match() );
static_assert( test( (char)0 ).did_not_match() );
static_assert( test( (int)0 ).did_not_match() );
static_assert( test( (char16_t)0 ).did_not_match() );
static_assert( test( (float)0 ).did_not_match() );
static_assert( test( (void*)0 ).did_not_match() );
static_assert( test( X{} ).did_not_match() );
enum E : unsigned { e };
static_assert( test( e ).did_not_match() );

#if !defined(__STRICT_ANSI__) && defined __SIZEOF_INT128__
static_assert( test( (unsigned __int128)0 ) );
static_assert( test( (__int128)0 ).did_not_match() );
#endif
#if defined(__GLIBCXX_TYPE_INT_N_0)
static_assert( test( (unsigned __GLIBCXX_TYPE_INT_N_0)0 ) );
static_assert( test( (__GLIBCXX_TYPE_INT_N_0)0 ).did_not_match() );
#endif
#if defined(__GLIBCXX_TYPE_INT_N_1)
static_assert( test( (unsigned __GLIBCXX_TYPE_INT_N_1)0 ) );
static_assert( test( (__GLIBCXX_TYPE_INT_N_1)0 ).did_not_match() );
#endif
#if defined(__GLIBCXX_TYPE_INT_N_2)
static_assert( test( (unsigned __GLIBCXX_TYPE_INT_N_2)0 ) );
static_assert( test( (__GLIBCXX_TYPE_INT_N_2)0 ).did_not_match() );
#endif
#if defined(__GLIBCXX_TYPE_INT_N_3)
static_assert( test( (unsigned __GLIBCXX_TYPE_INT_N_3)0 ) );
static_assert( test( (__GLIBCXX_TYPE_INT_N_3)0 ).did_not_match() );
#endif

#include <cstddef>
static_assert( test( (std::byte)0 ).did_not_match() );
