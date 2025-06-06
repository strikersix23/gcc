// { dg-do run { target c++17 } }
// { dg-require-effective-target std_allocator_new }
// { dg-require-effective-target c++20 { target powerpc-ibm-aix* } }

// Copyright (C) 2021-2025 Free Software Foundation, Inc.
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

// libstdc++/96088

#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include <testsuite_hooks.h>
#include <replacement_memory_operators.h>

static constexpr std::initializer_list<std::pair<const char*, const char*>> lst =
  { { "long_str_for_dynamic_allocation", "long_str_for_dynamic_allocation" } };

void
test01()
{
  __gnu_test::counter::reset();
  std::unordered_map<std::string, std::string> um;
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

void
test02()
{
  __gnu_test::counter::reset();
  std::unordered_map<std::string, std::string,
		     std::hash<std::string_view>,
		     std::equal_to<std::string_view>> um;
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

std::size_t
hash_string_f(const std::string& str) noexcept
{
  std::hash<std::string> h;
  return h(str);
}

void
test11()
{
  typedef std::size_t (*hash_string_t)(const std::string&) noexcept;
  __gnu_test::counter::reset();
  hash_string_t hasher = &hash_string_f;
  std::unordered_map<std::string, std::string,
		     hash_string_t,
		     std::equal_to<std::string>> um(0, hasher);
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

std::size_t
hash_string_view_f(const std::string_view& str) noexcept
{
  std::hash<std::string_view> h;
  return h(str);
}

void
test12()
{
  typedef std::size_t (*hash_stringview_t) (const std::string_view&) noexcept;
  __gnu_test::counter::reset();
  hash_stringview_t hasher = &hash_string_view_f;
  std::unordered_map<std::string, std::string, hash_stringview_t,
		     std::equal_to<std::string_view>> um(0, hasher);
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

struct hash_string_functor
{
  std::size_t
  operator()(const std::string& str) const noexcept
  {
    std::hash<std::string> h;
    return h(str);
  }
};

void
test21()
{
  __gnu_test::counter::reset();
  std::unordered_map<std::string, std::string,
		     hash_string_functor,
		     std::equal_to<std::string>> um;
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

struct hash_string_view_noexcept_functor
{
  std::size_t
  operator()(const std::string_view& str) const noexcept
  {
    std::hash<std::string_view> h;
    return h(str);
  }
};

void
test22()
{
  __gnu_test::counter::reset();
  std::unordered_map<std::string, std::string,
		     hash_string_view_noexcept_functor,
		     std::equal_to<std::string_view>> um;
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

struct hash_string_view_functor
{
  std::size_t
  operator()(const std::string_view& str) const
  {
    std::hash<std::string_view> h;
    return h(str);
  }
};

void
test23()
{
  __gnu_test::counter::reset();
  std::unordered_map<std::string, std::string,
		     hash_string_view_functor,
		     std::equal_to<std::string_view>> um;
  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  VERIFY( __gnu_test::counter::get()._M_increments == 4 );

  um.insert(lst.begin(), lst.end());
  VERIFY( um.size() == 1 );

  VERIFY( __gnu_test::counter::count() == 4 );
  // Allocated another node and a pair<const std::string, std::string>:
  VERIFY( __gnu_test::counter::get()._M_increments == 7 );
}

void
test03()
{
  std::vector<std::pair<std::string, std::string>> v;
  v.insert(v.end(), lst.begin(), lst.end());

  const auto origin = __gnu_test::counter::count();

  {
    __gnu_test::counter::reset();
    std::unordered_map<std::string, std::string,
		       std::hash<std::string_view>,
		       std::equal_to<std::string_view>> um;
    um.insert(v.begin(), v.end());
    VERIFY( um.size() == 1 );

    // Allocate array of buckets, a node, and the 2 std::string (unless COW).
    constexpr std::size_t increments = _GLIBCXX_USE_CXX11_ABI ? 4 : 2;

    VERIFY( __gnu_test::counter::count() == origin + increments );
    VERIFY( __gnu_test::counter::get()._M_increments == increments );

    um.insert(v.begin(), v.end());
    VERIFY( um.size() == 1 );

    VERIFY( __gnu_test::counter::count() == origin + increments );
    VERIFY( __gnu_test::counter::get()._M_increments == increments );
  }
  VERIFY( __gnu_test::counter::count() == origin );

  {
    __gnu_test::counter::reset();
    std::unordered_map<std::string, std::string,
		       std::hash<std::string_view>,
		       std::equal_to<std::string_view>> um;
    um.insert(std::make_move_iterator(v.begin()),
	      std::make_move_iterator(v.end()));
    VERIFY( um.size() == 1 );

    // Allocate array of buckets and a node. String is moved.
    constexpr std::size_t increments = 2;

    VERIFY( __gnu_test::counter::count() == origin + increments );
    VERIFY( __gnu_test::counter::get()._M_increments == increments );
  }
}

int
main()
{
  __gnu_test::counter::scope s;
  test01();
  test02();
  test11();
  test12();
  test21();
  test22();
  test03();
  return 0;
}
