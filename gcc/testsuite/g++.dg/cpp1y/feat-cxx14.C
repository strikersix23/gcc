// { dg-options "-std=c++14 -I${srcdir}/g++.dg/cpp1y -I${srcdir}/g++.dg/cpp1y/testinc" }
// { dg-skip-if "requires hosted libstdc++ for complex" { ! hostedlib } }

//  C++98 features:

#ifndef __cpp_rtti
#  error "__cpp_rtti"
#elif  __cpp_rtti != 199711
#  error "__cpp_rtti != 199711"
#endif

#ifndef __cpp_exceptions
#  error "__cpp_exceptions"
#elif  __cpp_exceptions != 199711
#  error "__cpp_exceptions != 199711"
#endif

//  C++11 features:

#ifndef __cpp_unicode_characters
#  error "__cpp_unicode_characters"
#elif __cpp_unicode_characters != 200704
#  error "__cpp_unicode_characters != 200704"
#endif

#ifndef __cpp_raw_strings
#  error "__cpp_raw_strings"
#elif __cpp_raw_strings != 200710
#  error "__cpp_raw_strings != 200710"
#endif

#ifndef __cpp_unicode_literals
#  error "__cpp_unicode_literals"
#elif __cpp_unicode_literals != 200710
#  error "__cpp_unicode_literals != 200710"
#endif

#ifndef __cpp_user_defined_literals
#  error "__cpp_user_defined_literals"
#elif __cpp_user_defined_literals != 200809
#  error "__cpp_user_defined_literals != 200809"
#endif

#ifndef __cpp_lambdas
#  error "__cpp_lambdas"
#elif __cpp_lambdas != 200907
#  error "__cpp_lambdas != 200907"
#endif

#ifndef __cpp_range_based_for
#  error "__cpp_range_based_for"
#elif __cpp_range_based_for < 200907
#  error "__cpp_range_based_for < 200907"
#endif

#ifndef __cpp_static_assert
#  error "__cpp_static_assert"
#elif __cpp_static_assert != 200410
#  error "__cpp_static_assert != 200410"
#endif

#ifndef __cpp_decltype
#  error "__cpp_decltype"
#elif __cpp_decltype != 200707
#  error "__cpp_decltype != 200707"
#endif

#ifndef __cpp_attributes
#  error "__cpp_attributes"
#elif __cpp_attributes != 200809
#  error "__cpp_attributes != 200809"
#endif

#ifndef __cpp_rvalue_references
#  error "__cpp_rvalue_references"
#elif __cpp_rvalue_references != 200610
#  error "__cpp_rvalue_references != 200610"
#endif

#ifndef __cpp_variadic_templates
#  error "__cpp_variadic_templates"
#elif __cpp_variadic_templates != 200704
#  error "__cpp_variadic_templates != 200704"
#endif

#ifndef __cpp_initializer_lists
#  error "__cpp_initializer_lists"
#elif __cpp_initializer_lists != 200806
#  error "__cpp_initializer_lists != 200806"
#endif

#ifndef __cpp_delegating_constructors
#  error "__cpp_delegating_constructors"
#elif __cpp_delegating_constructors != 200604
#  error "__cpp_delegating_constructors != 200604"
#endif

#ifndef __cpp_nsdmi
#  error "__cpp_nsdmi"
#elif __cpp_nsdmi != 200809
#  error "__cpp_nsdmi != 200809"
#endif

#ifndef __cpp_inheriting_constructors
#  error "__cpp_inheriting_constructors"
#elif  __cpp_inheriting_constructors!= 201511
#  error "__cpp_inheriting_constructors != 201511"
#endif

#ifndef __cpp_ref_qualifiers
#  error "__cpp_ref_qualifiers"
#elif __cpp_ref_qualifiers != 200710
#  error "__cpp_ref_qualifiers != 200710"
#endif

#ifndef __cpp_alias_templates
#  error "__cpp_alias_templates"
#elif __cpp_alias_templates != 200704
#  error "__cpp_alias_templates != 200704"
#endif

#ifndef __cpp_threadsafe_static_init
#  error "__cpp_threadsafe_static_init"
#elif __cpp_threadsafe_static_init != 200806
#  error "__cpp_threadsafe_static_init != 200806"
#endif

//  C++14 features:

#ifndef __cpp_binary_literals
#  error "__cpp_binary_literals"
#elif __cpp_binary_literals != 201304
#  error "__cpp_binary_literals != 201304"
#endif

#ifndef __cpp_init_captures
#  error "__cpp_init_captures"
#elif __cpp_init_captures != 201304
#  error "__cpp_init_captures != 201304"
#endif

#ifndef __cpp_generic_lambdas
#  error "__cpp_generic_lambdas"
#elif __cpp_generic_lambdas != 201304
#  error "__cpp_generic_lambdas != 201304"
#endif

#ifndef __cpp_constexpr
#  error "__cpp_constexpr"
#elif __cpp_constexpr != 201304
#  error "__cpp_constexpr != 201304"
#endif

#ifndef __cpp_decltype_auto
#  error "__cpp_decltype_auto"
#elif __cpp_decltype_auto != 201304
#  error "__cpp_decltype_auto != 201304"
#endif

#ifndef __cpp_return_type_deduction
#  error "__cpp_return_type_deduction"
#elif __cpp_return_type_deduction != 201304
#  error "__cpp_return_type_deduction != 201304"
#endif

#ifndef __cpp_aggregate_nsdmi
#  error "__cpp_aggregate_nsdmi"
#elif __cpp_aggregate_nsdmi != 201304
#  error "__cpp_aggregate_nsdmi != 201304"
#endif

#ifndef __cpp_variable_templates
#  error "__cpp_variable_templates"
#elif __cpp_variable_templates != 201304
#  error "__cpp_variable_templates != 201304"
#endif

#ifndef __cpp_digit_separators
#  error "__cpp_digit_separators"
#elif __cpp_digit_separators != 201309
#  error "__cpp_digit_separators != 201309"
#endif

#ifndef __cpp_sized_deallocation
#  error "__cpp_sized_deallocation"
#elif __cpp_sized_deallocation != 201309
#  error "__cpp_sized_deallocation != 201309"
#endif

//  GNU VLA support:

#ifndef __cpp_runtime_arrays
#  error "__cpp_runtime_arrays"
#elif __cpp_runtime_arrays != 198712
#  error "__cpp_runtime_arrays != 198712"
#endif

//  C++11 attributes:

#ifdef __has_cpp_attribute
#  if ! __has_cpp_attribute(noreturn)
#    error "__has_cpp_attribute(noreturn)"
#  elif __has_cpp_attribute(noreturn) != 200809
#    error "__has_cpp_attribute(noreturn) != 200809"
#  endif
#else
#  error "__has_cpp_attribute"
#endif

//  Attribute carries_dependency not in yet.
//#ifdef __has_cpp_attribute
//#  if ! __has_cpp_attribute(carries_dependency)
//#    error "__has_cpp_attribute(carries_dependency)"
//#  elif __has_cpp_attribute(carries_dependency) != 200809
//#    error "__has_cpp_attribute(carries_dependency) != 200809"
//#  endif
//#else
//#  error "__has_cpp_attribute"
//#endif

//  C++14 attributes:

#ifdef __has_cpp_attribute
#  if ! __has_cpp_attribute(deprecated)
#    error "__has_cpp_attribute(deprecated)"
#  elif __has_cpp_attribute(deprecated) != 201309
#    error "__has_cpp_attribute(deprecated) != 201309"
#  endif
#else
#  error "__has_cpp_attribute"
#endif

//  Include checks:

//  Check for __has_include macro.
#ifndef __has_include
#  error "__has_include"
#endif

//  Try known bracket header (use operator).
#if __has_include (<complex>)
#else
#  error "<complex>"
#endif

//  Define and use a macro to invoke the operator.
#define sluggo(TXT) __has_include(TXT)

#if sluggo(<complex>)
#else
#  error "<complex>"
#endif

#if ! sluggo(<complex>)
#  error "<complex>"
#else
#endif

//  Quoted complex.h should find at least the bracket version.
#if __has_include("complex.h")
#else
#  error "complex.h"
#endif

//  Try known local quote header.
#if __has_include("complex_literals.h")
#else
#  error "\"complex_literals.h\""
#endif

//  Try nonexistent bracket header.
#if __has_include(<stuff>)
#  error "<stuff>"
#else
#endif

//  Try nonexistent quote header.
#if __has_include("phlegm")
#  error "\"phlegm\""
#else
#endif

//  Test __has_include_next.
#if __has_include("phoobhar.h")
#  include "phoobhar.h"
#else
#  error "__has_include(\"phoobhar.h\")"
#endif

//  Try a macro.
#define COMPLEX_INC "complex.h"
#if __has_include(COMPLEX_INC)
#else
#  error COMPLEX_INC
#endif

//  Realistic use of __has_include.
#if __has_include(<array>)
#  define STD_ARRAY 1
#  include <array>
  template<typename _Tp, std::size_t _Num>
    using array = std::array<_Tp, _Num>;
#elif __has_include(<tr1/array>)
#  define TR1_ARRAY 1
#  include <tr1/array>
  template<typename _Tp, std::size_t _Num>
    typedef std::tr1::array<_Tp, _Num> array;
#endif
