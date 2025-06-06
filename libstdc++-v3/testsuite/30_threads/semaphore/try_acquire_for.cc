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
// { dg-additional-options "-pthread" { target pthread } }
// { dg-require-gthreads "" }
// { dg-add-options libatomic }

#include <semaphore>
#include <chrono>
#include <thread>
#include <atomic>
#include <testsuite_hooks.h>

void test01()
{
  using namespace std::chrono_literals;
  std::counting_semaphore<10> s(2);
  s.acquire();

  auto const dur = 250ms;
  {
    auto const t0 = std::chrono::steady_clock::now();
    VERIFY( s.try_acquire_for(dur) );
    auto const diff = std::chrono::steady_clock::now() - t0;
    VERIFY( diff < dur );
  }

  {
    auto const t0 = std::chrono::steady_clock::now();
    VERIFY( !s.try_acquire_for(dur) );
    auto const diff = std::chrono::steady_clock::now() - t0;
    VERIFY( diff >= dur );
  }
}

void test02()
{
  using namespace std::chrono_literals;
  std::binary_semaphore s(1);
  std::atomic<int> a(0), b(0);
  std::thread t([&] {
    a.wait(0);
    auto const dur = 250ms;
    VERIFY( !s.try_acquire_for(dur) );
    b++;
    b.notify_one();

    a.wait(1);
    VERIFY( s.try_acquire_for(dur) );
    b++;
    b.notify_one();
  });
  t.detach();

  s.acquire();
  a++;
  a.notify_one();
  b.wait(0);
  s.release();
  a++;
  a.notify_one();

  b.wait(1);
}

void
test03()
{
  using tick = std::chrono::system_clock::duration::period;
  using halftick = std::ratio<tick::num, 2 * tick::den>;
  std::chrono::duration<long long, halftick> timeout(1);
  std::binary_semaphore s(1);

  // Using higher resolution than chrono::system_clock should compile:
  s.try_acquire_for(timeout);
}

int main()
{
  test01();
  test02();
  test03();
}
