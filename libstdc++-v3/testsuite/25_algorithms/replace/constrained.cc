// Copyright (C) 2020-2025 Free Software Foundation, Inc.
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

// { dg-do run { target c++20 } }

#include <algorithm>
#include <testsuite_hooks.h>
#include <testsuite_iterators.h>

using __gnu_test::test_container;
using __gnu_test::test_range;
using __gnu_test::input_iterator_wrapper;
using __gnu_test::forward_iterator_wrapper;

namespace ranges = std::ranges;

struct X
{
  int i;

  friend constexpr bool
  operator==(const X& a, const X& b)
  {
    return a.i == b.i;
  }
};

void
test01()
{
    {
      X x[6] = { {2}, {2}, {6}, {8}, {10}, {11} };
      X y[6] = { {2}, {2}, {6}, {9}, {10}, {11} };
      auto res = ranges::replace(x, x+5, 8, X{9}, &X::i);
      VERIFY( res == x+5 );
      VERIFY( ranges::equal(x, y) );
    }

    {
      X x[6] = { {2}, {2}, {6}, {8}, {10}, {11} };
      X y[6] = { {2}, {2}, {6}, {8}, {10}, {11} };
      auto res = ranges::replace(x, x+5, 7, X{9}, &X::i);
      VERIFY( res == x+5 );
      VERIFY( ranges::equal(x, y) );
    }

    {
      X x[6] = { {2}, {2}, {6}, {8}, {10}, {11} };
      X y[6] = { {7}, {7}, {6}, {8}, {10}, {11} };
      test_container<X, forward_iterator_wrapper> cx(x), cy(y);
      auto res = ranges::replace(cx, 2, X{7}, &X::i);
      VERIFY( res == cx.end() );
      VERIFY( ranges::equal(cx, cy) );
    }

    {
      int x[6] = { {2}, {2}, {6}, {8}, {10}, {2} };
      int y[6] = { {7}, {7}, {6}, {8}, {10}, {7} };
      test_range<int, input_iterator_wrapper> rx(x), ry(y);
      auto res = ranges::replace(rx, 2, 7);
      VERIFY( res == rx.end() );

      rx.bounds.first = x;
      ry.bounds.first = y;
      VERIFY( ranges::equal(rx, ry) );
    }
}

struct Y { int i; int j; };

constexpr bool
test02()
{
  bool ok = true;
  Y x[] = { {3,2}, {2,4}, {3,6} };
  Y y[] = { {4,5}, {2,4}, {4,5} };
  auto res = ranges::replace(x, 3, Y{4,5}, &Y::i);
  ok &= res == x+3;
  ok &= ranges::equal(x, y, {}, &Y::i, &Y::i);
  ok &= ranges::equal(x, y, {}, &Y::j, &Y::j);
  return ok;
}

int
main()
{
  test01();
  static_assert(test02());
}
