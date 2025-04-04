// 1999-06-03 bkoz

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

// 21.3.5.4 basic_string::insert

#include <string>
#include <testsuite_hooks.h>

// More
//   wstring& insert(size_type __p, const wchar_t* s, size_type n);
//   wstring& insert(size_type __p, const wchar_t* s);
// but now s points inside the _Rep
void test02(void)
{
  std::wstring str01;
  const wchar_t* title = L"Everything was beautiful, and nothing hurt";
  // Increasing size: str01 is reallocated every time.
  str01 = title;
  str01.insert(0, str01.c_str() + str01.size() - 4, 4);
  VERIFY( str01 == L"hurtEverything was beautiful, and nothing hurt" );
  str01 = title;
  str01.insert(0, str01.c_str(), 5);
  VERIFY( str01 == L"EveryEverything was beautiful, and nothing hurt" );
  str01 = title;
  str01.insert(10, str01.c_str() + 4, 6);
  VERIFY( str01 == L"Everythingything was beautiful, and nothing hurt" );
  str01 = title;
  str01.insert(15, str01.c_str(), 10);
  VERIFY( str01 == L"Everything was Everythingbeautiful, and nothing hurt" );
  str01 = title;
  str01.insert(15, str01.c_str() + 11, 13);
  VERIFY( str01 == L"Everything was was beautifulbeautiful, and nothing hurt" );
  str01 = title;
  str01.insert(0, str01.c_str());
  VERIFY( str01 == L"Everything was beautiful, and nothing hurt"
	  L"Everything was beautiful, and nothing hurt");
  // Again: no reallocations.
  str01 = title;
  str01.insert(0, str01.c_str() + str01.size() - 4, 4);
  VERIFY( str01 == L"hurtEverything was beautiful, and nothing hurt" );
  str01 = title;
  str01.insert(0, str01.c_str(), 5);
  VERIFY( str01 == L"EveryEverything was beautiful, and nothing hurt" );
  str01 = title;
  str01.insert(10, str01.c_str() + 4, 6);
  VERIFY( str01 == L"Everythingything was beautiful, and nothing hurt" );
  str01 = title;
  str01.insert(15, str01.c_str(), 10);
  VERIFY( str01 == L"Everything was Everythingbeautiful, and nothing hurt" );
  str01 = title;
  str01.insert(15, str01.c_str() + 11, 13);
  VERIFY( str01 == L"Everything was was beautifulbeautiful, and nothing hurt" );
  str01 = title;
  str01.insert(0, str01.c_str());
  VERIFY( str01 == L"Everything was beautiful, and nothing hurt"
	  L"Everything was beautiful, and nothing hurt");
}

int main()
{ 
  test02();
  return 0;
}
