// Copyright (C) 1999-2025 Free Software Foundation, Inc.
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

// Test to see whether the host provides its own (inline) view of fabs.
// Origin: Kurt Garloff <kurt@garloff.de>, 2001-05-24
// { dg-do link }

#include <cmath>
#include <cstdio>

typedef double (*realfn) (double);

using std::fabs;

int main ()
{
  double a = fabs (-2.4);
  realfn myfn = fabs;
  double b = myfn (-2.5);
  std::printf ("%f, %f, %p\n", a, b, myfn);
  return 0;
}
