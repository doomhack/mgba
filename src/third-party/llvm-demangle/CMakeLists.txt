# LLVM's Itanium C++ demangler, used by the Qt frontend to show C++ names in profiles readably
add_library(llvm-demangle STATIC ItaniumDemangle.cpp)
target_include_directories(llvm-demangle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The demangler needs C++17, whatever standard the code using it is built with
set_target_properties(llvm-demangle PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
if(MSVC)
	# MSVC 19.51 (Visual Studio 18) miscompiles the demangler when it inlines, putting parentheses
	# around some parameter types: "big(int, (int))". Names are only demangled when a profile
	# loads, so inlining nothing costs nothing that matters.
	target_compile_options(llvm-demangle PRIVATE /Ob0)
endif()
